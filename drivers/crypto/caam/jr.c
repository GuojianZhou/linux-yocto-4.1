/*
 * CAAM/SEC 4.x transport/backend driver
 * JobR backend functionality
 *
 * Copyright 2008-2012 Freescale Semiconductor, Inc.
 */

#include <linux/of_irq.h>
#include <linux/of_address.h>

#include "compat.h"
#include "regs.h"
#include "jr.h"
#include "desc.h"
#include "intern.h"

struct jr_driver_data {
	/* List of Physical JobR's with the Driver */
	struct list_head	jr_list;
	spinlock_t		jr_alloc_lock;	/* jr_list lock */
} ____cacheline_aligned;

static struct jr_driver_data driver_data;

static int caam_reset_hw_jr(struct device *dev)
{
	struct caam_drv_private_jr *jrp = dev_get_drvdata(dev);
	unsigned int timeout = 100000;

	/*
	 * mask interrupts since we are going to poll
	 * for reset completion status
	 */
	setbits32(&jrp->rregs->rconfig_lo, JRCFG_IMSK);

	/* initiate flush (required prior to reset) */
	wr_reg32(&jrp->rregs->jrcommand, JRCR_RESET);
	while (((rd_reg32(&jrp->rregs->jrintstatus) & JRINT_ERR_HALT_MASK) ==
		JRINT_ERR_HALT_INPROGRESS) && --timeout)
		cpu_relax();

	if ((rd_reg32(&jrp->rregs->jrintstatus) & JRINT_ERR_HALT_MASK) !=
	    JRINT_ERR_HALT_COMPLETE || timeout == 0) {
		dev_err(dev, "failed to flush job ring %d\n", jrp->ridx);
		return -EIO;
	}

	/* initiate reset */
	timeout = 100000;
	wr_reg32(&jrp->rregs->jrcommand, JRCR_RESET);
	while ((rd_reg32(&jrp->rregs->jrcommand) & JRCR_RESET) && --timeout)
		cpu_relax();

	if (timeout == 0) {
		dev_err(dev, "failed to reset job ring %d\n", jrp->ridx);
		return -EIO;
	}

	/* unmask interrupts */
	clrbits32(&jrp->rregs->rconfig_lo, JRCFG_IMSK);

	return 0;
}

/*
 * Shutdown JobR independent of platform property code
 */
int caam_jr_shutdown(struct device *dev)
{
	struct caam_drv_private_jr *jrp = dev_get_drvdata(dev);
	int i, ret;

	ret = caam_reset_hw_jr(dev);

	for_each_possible_cpu(i) {
		napi_disable(per_cpu_ptr(jrp->irqtask, i));
		netif_napi_del(per_cpu_ptr(jrp->irqtask, i));
	}

	free_percpu(jrp->irqtask);
	free_percpu(jrp->net_dev);

	/* Release interrupt */
	free_irq(jrp->irq, dev);

	dma_free_coherent(dev, sizeof(dma_addr_t) * JOBR_DEPTH,
			  jrp->inpring, jrp->inpbusaddr);
	dma_free_coherent(dev, sizeof(struct jr_outentry) * JOBR_DEPTH,
			  jrp->outring, jrp->outbusaddr);
	kfree(jrp->entinfo);

	return ret;
}

static int caam_jr_remove(struct platform_device *pdev)
{
	int ret;
	struct device *jrdev;
	struct caam_drv_private_jr *jrpriv;

	jrdev = &pdev->dev;
	jrpriv = dev_get_drvdata(jrdev);

	/*
	 * Return EBUSY if job ring already allocated.
	 */
	if (atomic_read(&jrpriv->tfm_count)) {
		dev_err(jrdev, "Device is busy\n");
		return -EBUSY;
	}

	/* Remove the node from Physical JobR list maintained by driver */
	spin_lock(&driver_data.jr_alloc_lock);
	list_del(&jrpriv->list_node);
	spin_unlock(&driver_data.jr_alloc_lock);

	/* Release ring */
	ret = caam_jr_shutdown(jrdev);
	if (ret)
		dev_err(jrdev, "Failed to shut down job ring\n");
	irq_dispose_mapping(jrpriv->irq);

	return ret;
}

/* Main per-ring interrupt handler */
static irqreturn_t caam_jr_interrupt(int irq, void *st_dev)
{
	struct device *dev = st_dev;
	struct caam_drv_private_jr *jrp = dev_get_drvdata(dev);
	u32 irqstate;

	/*
	 * Check the output ring for ready responses, kick
	 * tasklet if jobs done.
	 */
	irqstate = rd_reg32(&jrp->rregs->jrintstatus);
	if (!irqstate)
		return IRQ_NONE;

	/*
	 * If JobR error, we got more development work to do
	 * Flag a bug now, but we really need to shut down and
	 * restart the queue (and fix code).
	 */
	if (irqstate & JRINT_JR_ERROR) {
		dev_err(dev, "job ring error: irqstate: %08x\n", irqstate);
		BUG();
	}

	/* mask valid interrupts */
	setbits32(&jrp->rregs->rconfig_lo, JRCFG_IMSK);

	/* Have valid interrupt at this point, just ACK and trigger */
	wr_reg32(&jrp->rregs->jrintstatus, irqstate);

	preempt_disable();
	napi_schedule(per_cpu_ptr(jrp->irqtask, smp_processor_id()));
	preempt_enable();

	return IRQ_HANDLED;
}

/* Consume the processed output ring Job */
static inline void caam_jr_consume(struct device *dev)
{
	int hw_idx, sw_idx, i, head, tail;
	struct caam_drv_private_jr *jrp = dev_get_drvdata(dev);
	void (*usercall)(struct device *dev, u32 *desc, u32 status, void *arg);
	u32 *userdesc, userstatus;
	void *userarg;

	head = ACCESS_ONCE(jrp->head);
	spin_lock(&jrp->outlock);

	sw_idx = jrp->tail;
	tail = jrp->tail;
	hw_idx = jrp->out_ring_read_index;

	for (i = 0; CIRC_CNT(head, tail + i, JOBR_DEPTH) >= 1; i++) {
		sw_idx = (tail + i) & (JOBR_DEPTH - 1);

		/* Read Barrier for desc address comparision */
		smp_read_barrier_depends();
		if (jrp->outring[hw_idx].desc ==
		    jrp->entinfo[sw_idx].desc_addr_dma)
			break; /* found */
	}
	/* we should never fail to find a matching descriptor */
	BUG_ON(CIRC_CNT(head, tail + i, JOBR_DEPTH) <= 0);

	/* Unmap just-run descriptor so we can post-process */
	dma_unmap_single(dev, jrp->outring[hw_idx].desc,
			 jrp->entinfo[sw_idx].desc_size,
			 DMA_TO_DEVICE);

	/* mark completed, avoid matching on a recycled desc addr */
	jrp->entinfo[sw_idx].desc_addr_dma = 0;

	/* Stash callback params for use outside of lock */
	usercall = jrp->entinfo[sw_idx].callbk;
	userarg = jrp->entinfo[sw_idx].cbkarg;
	userdesc = jrp->entinfo[sw_idx].desc_addr_virt;
	userstatus = jrp->outring[hw_idx].jrstatus;

	/* set done */
	wr_reg32(&jrp->rregs->outring_rmvd, 1);

	jrp->out_ring_read_index = (jrp->out_ring_read_index + 1) &
				   (JOBR_DEPTH - 1);

	/*
	 * if this job completed out-of-order, do not increment
	 * the tail.  Otherwise, increment tail by 1 plus the
	 * number of subsequent jobs already completed out-of-order
	 */
	if (sw_idx == tail) {
		do {
			tail = (tail + 1) & (JOBR_DEPTH - 1);
		} while (CIRC_CNT(head, tail, JOBR_DEPTH) >= 1 &&
			 jrp->entinfo[tail].desc_addr_dma == 0);
		jrp->tail = tail;
	}

	spin_unlock(&jrp->outlock);

	/* Finally, execute user's callback */
	usercall(dev, userdesc, userstatus, userarg);
}

/* Deferred service handler, run as interrupt-fired tasklet */
static int caam_jr_dequeue(struct napi_struct *napi, int budget)
{
	struct device *dev = &napi->dev->dev;
	struct caam_drv_private_jr *jrp = dev_get_drvdata(dev);
	int cleaned = 0;

	dev = jrp->dev;
	while (rd_reg32(&jrp->rregs->outring_used) && cleaned < budget) {
		caam_jr_consume(dev);
		cleaned++;
	}

	if (cleaned < budget) {
		napi_complete(per_cpu_ptr(jrp->irqtask, smp_processor_id()));
		/* reenable / unmask IRQs */
		clrbits32(&jrp->rregs->rconfig_lo, JRCFG_IMSK);
	}

	return cleaned;
}

/**
 * caam_jr_alloc() - Alloc a job ring for someone to use as needed.
 *
 * returns :  pointer to the newly allocated physical
 *	      JobR dev can be written to if successful.
 **/
struct device *caam_jr_alloc(void)
{
	struct caam_drv_private_jr *jrpriv, *min_jrpriv = NULL;
	struct device *dev = ERR_PTR(-ENODEV);
	int min_tfm_cnt	= INT_MAX;
	int tfm_cnt;

	spin_lock(&driver_data.jr_alloc_lock);

	if (list_empty(&driver_data.jr_list)) {
		spin_unlock(&driver_data.jr_alloc_lock);
		return ERR_PTR(-ENODEV);
	}

	list_for_each_entry(jrpriv, &driver_data.jr_list, list_node) {
		tfm_cnt = atomic_read(&jrpriv->tfm_count);
		if (tfm_cnt < min_tfm_cnt) {
			min_tfm_cnt = tfm_cnt;
			min_jrpriv = jrpriv;
		}
		if (!min_tfm_cnt)
			break;
	}

	if (min_jrpriv) {
		atomic_inc(&min_jrpriv->tfm_count);
		dev = min_jrpriv->dev;
	}
	spin_unlock(&driver_data.jr_alloc_lock);

	return dev;
}
EXPORT_SYMBOL(caam_jr_alloc);

/**
 * caam_jr_free() - Free the Job Ring
 * @rdev     - points to the dev that identifies the Job ring to
 *             be released.
 **/
void caam_jr_free(struct device *rdev)
{
	struct caam_drv_private_jr *jrpriv = dev_get_drvdata(rdev);

	atomic_dec(&jrpriv->tfm_count);
}
EXPORT_SYMBOL(caam_jr_free);

/**
 * caam_jr_enqueue() - Enqueue a job descriptor head. Returns 0 if OK,
 * -EBUSY if the queue is full, -EIO if it cannot map the caller's
 * descriptor.
 * @dev:  device of the job ring to be used. This device should have
 *        been assigned prior by caam_jr_register().
 * @desc: points to a job descriptor that execute our request. All
 *        descriptors (and all referenced data) must be in a DMAable
 *        region, and all data references must be physical addresses
 *        accessible to CAAM (i.e. within a PAMU window granted
 *        to it).
 * @cbk:  pointer to a callback function to be invoked upon completion
 *        of this request. This has the form:
 *        callback(struct device *dev, u32 *desc, u32 stat, void *arg)
 *        where:
 *        @dev:    contains the job ring device that processed this
 *                 response.
 *        @desc:   descriptor that initiated the request, same as
 *                 "desc" being argued to caam_jr_enqueue().
 *        @status: untranslated status received from CAAM. See the
 *                 reference manual for a detailed description of
 *                 error meaning, or see the JRSTA definitions in the
 *                 register header file
 *        @areq:   optional pointer to an argument passed with the
 *                 original request
 * @areq: optional pointer to a user argument for use at callback
 *        time.
 **/
int caam_jr_enqueue(struct device *dev, u32 *desc,
		    void (*cbk)(struct device *dev, u32 *desc,
				u32 status, void *areq),
		    void *areq)
{
	struct caam_drv_private_jr *jrp = dev_get_drvdata(dev);
	struct caam_jrentry_info *head_entry;
	int head, tail, desc_size;
	dma_addr_t desc_dma;

	desc_size = (*desc & HDR_JD_LENGTH_MASK) * sizeof(u32);
	desc_dma = dma_map_single(dev, desc, desc_size, DMA_TO_DEVICE);
	if (dma_mapping_error(dev, desc_dma)) {
		dev_err(dev, "caam_jr_enqueue(): can't map jobdesc\n");
		return -EIO;
	}

	spin_lock_bh(&jrp->inplock);

	head = jrp->head;
	tail = ACCESS_ONCE(jrp->tail);

	if (!rd_reg32(&jrp->rregs->inpring_avail) ||
	    CIRC_SPACE(head, tail, JOBR_DEPTH) <= 0) {
		spin_unlock_bh(&jrp->inplock);
		dma_unmap_single(dev, desc_dma, desc_size, DMA_TO_DEVICE);
		return -EBUSY;
	}

	head_entry = &jrp->entinfo[head];
	head_entry->desc_addr_virt = desc;
	head_entry->desc_size = desc_size;
	head_entry->callbk = (void *)cbk;
	head_entry->cbkarg = areq;
	head_entry->desc_addr_dma = desc_dma;

	jrp->inpring[jrp->inp_ring_write_index] = desc_dma;

	smp_wmb();

	jrp->inp_ring_write_index = (jrp->inp_ring_write_index + 1) &
				    (JOBR_DEPTH - 1);
	jrp->head = (head + 1) & (JOBR_DEPTH - 1);

	wr_reg32(&jrp->rregs->inpring_jobadd, 1);

	spin_unlock_bh(&jrp->inplock);

	return 0;
}
EXPORT_SYMBOL(caam_jr_enqueue);

#ifdef CONFIG_PM
/* Return Failure for Job pending in input ring */
static void caam_fail_inpjobs(struct device *dev)
{
	struct caam_drv_private_jr *jrp = dev_get_drvdata(dev);
	void (*usercall)(struct device *dev, u32 *desc, u32 status, void *arg);
	u32 *userdesc;
	void *userarg;
	int sw_idx;

	/* Check for jobs left after reaching output ring and return error */
	for (sw_idx = 0; sw_idx < JOBR_DEPTH; sw_idx++) {
		if (jrp->entinfo[sw_idx].desc_addr_dma) {
			usercall = jrp->entinfo[sw_idx].callbk;
			userarg = jrp->entinfo[sw_idx].cbkarg;
			userdesc = jrp->entinfo[sw_idx].desc_addr_virt;
			usercall(dev, userdesc, -EIO, userarg);
			jrp->entinfo[sw_idx].desc_addr_dma = 0;
		}
	}
}

/* Suspend handler for Job Ring */
static int jr_suspend(struct device *dev)
{
	struct caam_drv_private_jr *jrp = dev_get_drvdata(dev);
	unsigned int timeout = 100000;
	int ret = 0;

	/*
	 * mask interrupts since we are going to poll
	 * for reset completion status
	 */
	setbits32(&jrp->rregs->rconfig_lo, JRCFG_IMSK);

	/*
	 * Process all the pending completed Jobs to make room for
	 * Job's coming to Outring during flush. This gives caam
	 * some more space in outring and process some more Job's
	 */
	while (rd_reg32(&jrp->rregs->outring_used))
		caam_jr_consume(dev);

	/* initiate flush (required prior to reset) */
	wr_reg32(&jrp->rregs->jrcommand, JRCR_RESET);
	while (((rd_reg32(&jrp->rregs->jrintstatus) & JRINT_ERR_HALT_MASK) ==
		JRINT_ERR_HALT_INPROGRESS) && --timeout)
		cpu_relax();

	if ((rd_reg32(&jrp->rregs->jrintstatus) & JRINT_ERR_HALT_MASK) !=
	    JRINT_ERR_HALT_COMPLETE || timeout == 0) {
		dev_err(dev, "failed to flush job ring %d\n", jrp->ridx);
		ret = -EIO;
		goto err;
	}

	/*
	 * Disallow any further addition in Job Ring by making input_ring
	 * size ZERO. If output complete ring processing try to enqueue
	 * more Job's back to JR, it will return -EBUSY
	 */
	wr_reg32(&jrp->rregs->inpring_size, 0);

	while (rd_reg32(&jrp->rregs->outring_used))
		caam_jr_consume(dev);

	/* Its too late, now newely added jobs will be failed */
	caam_fail_inpjobs(dev);

	/* initiate reset */
	timeout = 100000;
	wr_reg32(&jrp->rregs->jrcommand, JRCR_RESET);
	while ((rd_reg32(&jrp->rregs->jrcommand) & JRCR_RESET) && --timeout)
		cpu_relax();

	if (timeout == 0) {
		dev_err(dev, "failed to reset job ring %d\n", jrp->ridx);
		ret = -EIO;
		goto err;
	}

err:
	/* unmask interrupts */
	clrbits32(&jrp->rregs->rconfig_lo, JRCFG_IMSK);
	return ret;
}

/* Resume handler for Job Ring */
static int jr_resume(struct device *dev)
{
	struct caam_drv_private_jr *jrp;

	jrp = dev_get_drvdata(dev);

	memset(jrp->entinfo, 0, sizeof(struct caam_jrentry_info) * JOBR_DEPTH);

	/* Setup rings */
	jrp->inp_ring_write_index = 0;
	jrp->out_ring_read_index = 0;
	jrp->head = 0;
	jrp->tail = 0;

	/* Setup ring base registers */
	wr_reg64(&jrp->rregs->inpring_base, jrp->inpbusaddr);
	wr_reg64(&jrp->rregs->outring_base, jrp->outbusaddr);
	/* Setup ring size */
	wr_reg32(&jrp->rregs->inpring_size, JOBR_DEPTH);
	wr_reg32(&jrp->rregs->outring_size, JOBR_DEPTH);

	setbits32(&jrp->rregs->rconfig_lo, JOBR_INTC |
		  (JOBR_INTC_COUNT_THLD << JRCFG_ICDCT_SHIFT) |
		  (JOBR_INTC_TIME_THLD << JRCFG_ICTT_SHIFT));
	return 0;
}

const struct dev_pm_ops jr_pm_ops = {
	.suspend = jr_suspend,
	.resume = jr_resume,
};
#endif /* CONFIG_PM */

/*
 * Init JobR independent of platform property detection
 */
static int caam_jr_init(struct device *dev)
{
	struct caam_drv_private_jr *jrp;
	int i, error;

	jrp = dev_get_drvdata(dev);

	/* Connect job ring interrupt handler. */
	jrp->irqtask = alloc_percpu(struct napi_struct);
	if (!jrp->irqtask) {
		dev_err(dev, "can't allocate percpu irqtask memory\n");
		return -ENOMEM;
	}

	jrp->net_dev = alloc_percpu(struct net_device);
	if (!jrp->net_dev) {
		dev_err(dev, "can't allocate percpu net_dev memory\n");
		free_percpu(jrp->irqtask);
		return -ENOMEM;
	}

	for_each_possible_cpu(i) {
		(per_cpu_ptr(jrp->net_dev, i))->dev = *dev;
		INIT_LIST_HEAD(&per_cpu_ptr(jrp->net_dev, i)->napi_list);
		netif_napi_add(per_cpu_ptr(jrp->net_dev, i),
				per_cpu_ptr(jrp->irqtask, i),
				caam_jr_dequeue, CAAM_NAPI_WEIGHT);
		napi_enable(per_cpu_ptr(jrp->irqtask, i));
	}

	/* Connect job ring interrupt handler. */
	error = request_irq(jrp->irq, caam_jr_interrupt, IRQF_SHARED,
			    dev_name(dev), dev);
	if (error) {
		dev_err(dev, "can't connect JobR %d interrupt (%d)\n",
			jrp->ridx, jrp->irq);
		goto out_kill_deq;
	}

	error = caam_reset_hw_jr(dev);
	if (error)
		goto out_free_irq;

	error = -ENOMEM;
	jrp->inpring = dma_alloc_coherent(dev, sizeof(dma_addr_t) * JOBR_DEPTH,
					  &jrp->inpbusaddr, GFP_KERNEL);
	if (!jrp->inpring)
		goto out_free_irq;

	jrp->outring = dma_alloc_coherent(dev, sizeof(struct jr_outentry) *
					  JOBR_DEPTH, &jrp->outbusaddr,
					  GFP_KERNEL);
	if (!jrp->outring)
		goto out_free_inpring;

	jrp->entinfo = kzalloc(sizeof(struct caam_jrentry_info) * JOBR_DEPTH,
			       GFP_KERNEL);
	if (!jrp->entinfo)
		goto out_free_outring;

	/* Setup rings */
	jrp->inp_ring_write_index = 0;
	jrp->out_ring_read_index = 0;
	jrp->head = 0;
	jrp->tail = 0;

	wr_reg64(&jrp->rregs->inpring_base, jrp->inpbusaddr);
	wr_reg64(&jrp->rregs->outring_base, jrp->outbusaddr);
	wr_reg32(&jrp->rregs->inpring_size, JOBR_DEPTH);
	wr_reg32(&jrp->rregs->outring_size, JOBR_DEPTH);

	jrp->ringsize = JOBR_DEPTH;

	spin_lock_init(&jrp->inplock);
	spin_lock_init(&jrp->outlock);

	/* Select interrupt coalescing parameters */
	setbits32(&jrp->rregs->rconfig_lo, JOBR_INTC |
		  (JOBR_INTC_COUNT_THLD << JRCFG_ICDCT_SHIFT) |
		  (JOBR_INTC_TIME_THLD << JRCFG_ICTT_SHIFT));

	return 0;

out_free_outring:
	dma_free_coherent(dev, sizeof(struct jr_outentry) * JOBR_DEPTH,
			  jrp->outring, jrp->outbusaddr);
out_free_inpring:
	dma_free_coherent(dev, sizeof(dma_addr_t) * JOBR_DEPTH,
			  jrp->inpring, jrp->inpbusaddr);
	dev_err(dev, "can't allocate job rings for %d\n", jrp->ridx);
out_free_irq:
	free_irq(jrp->irq, dev);
out_kill_deq:
	for_each_possible_cpu(i) {
		napi_disable(per_cpu_ptr(jrp->irqtask, i));
		netif_napi_del(per_cpu_ptr(jrp->irqtask, i));
	}
	free_percpu(jrp->irqtask);
	free_percpu(jrp->net_dev);

	return error;
}


/*
 * Probe routine for each detected JobR subsystem.
 */
static int caam_jr_probe(struct platform_device *pdev)
{
	struct device *jrdev;
	struct device_node *nprop;
	struct caam_job_ring __iomem *ctrl;
	struct caam_drv_private_jr *jrpriv;
	static int total_jobrs;
	int error;

	jrdev = &pdev->dev;
	jrpriv = devm_kmalloc(jrdev, sizeof(struct caam_drv_private_jr),
			      GFP_KERNEL);
	if (!jrpriv)
		return -ENOMEM;

	dev_set_drvdata(jrdev, jrpriv);

	/* save ring identity relative to detection */
	jrpriv->ridx = total_jobrs++;

	nprop = pdev->dev.of_node;
	/* Get configuration properties from device tree */
	/* First, get register page */
	ctrl = of_iomap(nprop, 0);
	if (!ctrl) {
		dev_err(jrdev, "of_iomap() failed\n");
		return -ENOMEM;
	}

	jrpriv->rregs = (struct caam_job_ring __force *)ctrl;

	if (sizeof(dma_addr_t) == sizeof(u64))
		if (of_device_is_compatible(nprop, "fsl,sec-v5.0-job-ring"))
			dma_set_mask_and_coherent(jrdev, DMA_BIT_MASK(40));
		else
			dma_set_mask_and_coherent(jrdev, DMA_BIT_MASK(36));
	else
		dma_set_mask_and_coherent(jrdev, DMA_BIT_MASK(32));

	/* Identify the interrupt */
	jrpriv->irq = irq_of_parse_and_map(nprop, 0);

	/* Now do the platform independent part */
	error = caam_jr_init(jrdev); /* now turn on hardware */
	if (error) {
		irq_dispose_mapping(jrpriv->irq);
		return error;
	}

	jrpriv->dev = jrdev;
	spin_lock(&driver_data.jr_alloc_lock);
	list_add_tail(&jrpriv->list_node, &driver_data.jr_list);
	spin_unlock(&driver_data.jr_alloc_lock);

	atomic_set(&jrpriv->tfm_count, 0);

	return 0;
}

static struct of_device_id caam_jr_match[] = {
	{
		.compatible = "fsl,sec-v4.0-job-ring",
	},
	{
		.compatible = "fsl,sec4.0-job-ring",
	},
	{},
};
MODULE_DEVICE_TABLE(of, caam_jr_match);

static struct platform_driver caam_jr_driver = {
	.driver = {
		.name = "caam_jr",
		.of_match_table = caam_jr_match,
#ifdef CONFIG_PM
		.pm = &jr_pm_ops,
#endif
	},
	.probe       = caam_jr_probe,
	.remove      = caam_jr_remove,
};

static int __init jr_driver_init(void)
{
	spin_lock_init(&driver_data.jr_alloc_lock);
	INIT_LIST_HEAD(&driver_data.jr_list);
	return platform_driver_register(&caam_jr_driver);
}

static void __exit jr_driver_exit(void)
{
	platform_driver_unregister(&caam_jr_driver);
}

module_init(jr_driver_init);
module_exit(jr_driver_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FSL CAAM JR request backend");
MODULE_AUTHOR("Freescale Semiconductor - NMG/STC");
