// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018-2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#define pr_fmt(fmt) "cpu_input_boost: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/moduleparam.h>
#include <linux/msm_drm_notify.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/event_tracking.h>

/* The sched_param struct is located elsewhere in newer kernels */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
#include <uapi/linux/sched/types.h>
#endif

static unsigned int input_boost_freq_little __read_mostly =
	CONFIG_INPUT_BOOST_FREQ_LP;
static unsigned int input_boost_freq_big __read_mostly =
	CONFIG_INPUT_BOOST_FREQ_PERF;
static unsigned int input_boost_freq_prime __read_mostly =
	CONFIG_INPUT_BOOST_FREQ_PERFP;
static unsigned int max_boost_freq_little __read_mostly =
	CONFIG_MAX_BOOST_FREQ_LP;
static unsigned int max_boost_freq_big __read_mostly =
	CONFIG_MAX_BOOST_FREQ_PERF;
static unsigned int max_boost_freq_prime __read_mostly =
	CONFIG_MAX_BOOST_FREQ_PERFP;
static unsigned int cpu_freq_min_little __read_mostly =
	CONFIG_CPU_FREQ_MIN_LP;
static unsigned int cpu_freq_min_big __read_mostly =
	CONFIG_CPU_FREQ_MIN_PERF;
static unsigned int cpu_freq_min_prime __read_mostly =
	CONFIG_CPU_FREQ_MIN_PERFP;
static unsigned int cpu_freq_idle_little __read_mostly =
	CONFIG_CPU_FREQ_IDLE_LP;

static unsigned short input_boost_duration __read_mostly =
	CONFIG_INPUT_BOOST_DURATION_MS;
static unsigned short wake_boost_duration __read_mostly =
	CONFIG_WAKE_BOOST_DURATION_MS;

module_param(input_boost_freq_little, uint, 0644);
module_param(input_boost_freq_big, uint, 0644);
module_param(input_boost_freq_prime, uint, 0644);
module_param(max_boost_freq_little, uint, 0644);
module_param(max_boost_freq_big, uint, 0644);
module_param(max_boost_freq_prime, uint, 0644);
module_param(cpu_freq_min_little, uint, 0644);
module_param(cpu_freq_min_big, uint, 0644);
module_param(cpu_freq_min_prime, uint, 0644);
module_param(cpu_freq_idle_little, uint, 0644);

module_param(input_boost_duration, short, 0644);
module_param(wake_boost_duration, short, 0644);

#if(CONFIG_INPUT_BOOST_DURATION_MS != 0)
unsigned long last_input_time;
#endif
unsigned long last_mb_time;

enum {
	SCREEN_ON,
	INPUT_BOOST,
	MAX_BOOST
};

struct boost_drv {
	struct delayed_work input_unboost;
	struct delayed_work max_unboost;
	struct notifier_block cpu_notif;
	struct notifier_block msm_drm_notif;
	wait_queue_head_t boost_waitq;
	atomic_long_t max_boost_expires;
	unsigned long state;
};

static void input_unboost_worker(struct work_struct *work);
static void max_unboost_worker(struct work_struct *work);

static struct boost_drv boost_drv_g __read_mostly = {
	.input_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.input_unboost,
						    input_unboost_worker, 0),
	.max_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.max_unboost,
						  max_unboost_worker, 0),
	.boost_waitq = __WAIT_QUEUE_HEAD_INITIALIZER(boost_drv_g.boost_waitq)
};

extern int kp_active_mode(void);
static unsigned int get_input_boost_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, cpu_lp_mask))
		freq = input_boost_freq_little;
	else if (cpumask_test_cpu(policy->cpu, cpu_perf_mask))
		freq = input_boost_freq_big;
	else
		freq = input_boost_freq_prime;
	return min(freq, policy->max);
}

static unsigned int get_max_boost_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, cpu_lp_mask))
		freq = max_boost_freq_little;
	else if (cpumask_test_cpu(policy->cpu, cpu_perf_mask))
		freq = max_boost_freq_big;
	else
		freq = max_boost_freq_prime;
	return min(freq, policy->max);
}

static unsigned int get_min_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, cpu_lp_mask))
		freq = cpu_freq_min_little;

	if (cpumask_test_cpu(policy->cpu, cpu_perf_mask))
		freq = cpu_freq_min_big;
	else if (cpumask_test_cpu(policy->cpu, cpu_prime_mask))
		freq = cpu_freq_min_prime;

	return max(freq, policy->cpuinfo.min_freq);
}

static unsigned int get_idle_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, cpu_lp_mask))
		freq = cpu_freq_idle_little;
	else if (cpumask_test_cpu(policy->cpu, cpu_perf_mask))
		freq = CONFIG_CPU_FREQ_IDLE_PERF;
	else
		freq = CONFIG_CPU_FREQ_IDLE_PERFP;

	return max(freq, policy->cpuinfo.min_freq);
}


static void update_online_cpu_policy(void)
{
	unsigned int cpu;

	/* Only one CPU from each cluster needs to be updated */
	get_online_cpus();
	cpu = cpumask_first_and(cpu_lp_mask, cpu_online_mask);
	cpufreq_update_policy(cpu);
	cpu = cpumask_first_and(cpu_perf_mask, cpu_online_mask);
	cpufreq_update_policy(cpu);
	cpu = cpumask_first_and(cpu_prime_mask, cpu_online_mask);
	cpufreq_update_policy(cpu);
	put_online_cpus();
}

static void __cpu_input_boost_kick(struct boost_drv *b)
{
	unsigned int multi = 1;

	if (!test_bit(SCREEN_ON, &b->state))
		return;

	if (!input_boost_duration)
		return;

	if (kp_active_mode() == 1 && !test_bit(INPUT_BOOST, &b->state))
		return;

	set_bit(INPUT_BOOST, &b->state);
	if (!mod_delayed_work(system_unbound_wq, &b->input_unboost,
			      msecs_to_jiffies(input_boost_duration * multi)))
		wake_up(&b->boost_waitq);
}

void cpu_input_boost_kick(void)
{
	struct boost_drv *b = &boost_drv_g;

	__cpu_input_boost_kick(b);
}

static void __cpu_input_boost_kick_max(struct boost_drv *b,
				       unsigned int duration_ms,
				       bool always)
{
	unsigned long boost_jiffies = msecs_to_jiffies(duration_ms);
	unsigned long curr_expires, new_expires;

	if (!test_bit(SCREEN_ON, &b->state))
		return;

	if (kp_active_mode() == 1 && !always && !test_bit(MAX_BOOST, &b->state))
		return;

	do {
		curr_expires = atomic_long_read(&b->max_boost_expires);
		new_expires = jiffies + boost_jiffies;

		/* Skip this boost if there's a longer boost in effect */
		if (time_after(curr_expires, new_expires))
			return;
	} while (atomic_long_cmpxchg(&b->max_boost_expires, curr_expires,
				     new_expires) != curr_expires);

	set_bit(MAX_BOOST, &b->state);
	if (!mod_delayed_work(system_unbound_wq, &b->max_unboost,
			      boost_jiffies))
		wake_up(&b->boost_waitq);
}

void cpu_input_boost_kick_max(unsigned int duration_ms, bool always)
{
	struct boost_drv *b = &boost_drv_g;

	__cpu_input_boost_kick_max(b, duration_ms, always);
	last_mb_time = jiffies;
}

static void input_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), input_unboost);

	clear_bit(INPUT_BOOST, &b->state);
	wake_up(&b->boost_waitq);
}

static void max_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), max_unboost);

	clear_bit(MAX_BOOST, &b->state);
	wake_up(&b->boost_waitq);
}

static int cpu_boost_thread(void *data)
{
	struct boost_drv *b = data;
	unsigned long old_state = 0;

	sched_set_fifo(current);

	while (1) {
		bool should_stop = false;
		unsigned long curr_state;

		wait_event(b->boost_waitq,
			(curr_state = READ_ONCE(b->state)) != old_state ||
			(should_stop = kthread_should_stop()));

		if (should_stop)
			break;

		old_state = curr_state;
		update_online_cpu_policy();
	}

	return 0;
}

static int cpu_notifier_cb(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), cpu_notif);
	struct cpufreq_policy *policy = data;

	if (action != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	/* Unboost when the screen is off */
	if (!test_bit(SCREEN_ON, &b->state)) {
		policy->min = get_idle_freq(policy);
		return NOTIFY_OK;
	}

	/* Boost CPU to max frequency for max boost */
	if (test_bit(MAX_BOOST, &b->state)) {
		policy->min = get_max_boost_freq(policy);
		return NOTIFY_OK;
	}

	/*
	 * Boost to policy->max if the boost frequency is higher. When
	 * unboosting, set policy->min to the absolute min freq for the CPU.
	 */
	if (test_bit(INPUT_BOOST, &b->state))
		policy->min = get_input_boost_freq(policy);
	else
		policy->min = get_min_freq(policy);

	return NOTIFY_OK;
}

static int msm_drm_notifier_cb(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), msm_drm_notif);
	struct msm_drm_notifier *evdata = data;
	int *blank = evdata->data;

	/* Parse framebuffer blank events as soon as they occur */
	if (action != MSM_DRM_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	/* Boost when the screen turns on and unboost when it turns off */
	if (*blank == MSM_DRM_BLANK_UNBLANK_CUST) {
		set_bit(SCREEN_ON, &b->state);
		__cpu_input_boost_kick_max(b, wake_boost_duration, true);
	} else if (*blank == MSM_DRM_BLANK_POWERDOWN_CUST) {
		clear_bit(SCREEN_ON, &b->state);
		wake_up(&b->boost_waitq);
	}

	return NOTIFY_OK;
}

static void cpu_input_boost_input_event(struct input_handle *handle,
					unsigned int type, unsigned int code,
					int value)
{
	struct boost_drv *b = handle->handler->private;

	__cpu_input_boost_kick(b);

#if(CONFIG_INPUT_BOOST_DURATION_MS != 0)
	last_input_time = jiffies;
#endif
}

static int cpu_input_boost_input_connect(struct input_handler *handler,
					 struct input_dev *dev,
					 const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpu_input_boost_handle";

	ret = input_register_handle(handle);
	if (ret)
		goto free_handle;

	ret = input_open_device(handle);
	if (ret)
		goto unregister_handle;

	return 0;

unregister_handle:
	input_unregister_handle(handle);
free_handle:
	kfree(handle);
	return ret;
}

static void cpu_input_boost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpu_input_boost_ids[] = {
	/* Multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) }
	},
	/* Touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) }
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) }
	},
	{ }
};

static struct input_handler cpu_input_boost_input_handler = {
	.event		= cpu_input_boost_input_event,
	.connect	= cpu_input_boost_input_connect,
	.disconnect	= cpu_input_boost_input_disconnect,
	.name		= "cpu_input_boost_handler",
	.id_table	= cpu_input_boost_ids
};

static int __init cpu_input_boost_init(void)
{
	struct boost_drv *b = &boost_drv_g;
	struct task_struct *thread;
	int ret;

	set_bit(SCREEN_ON, &b->state);

	b->cpu_notif.notifier_call = cpu_notifier_cb;
	ret = cpufreq_register_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_debug("Failed to register cpufreq notifier, err: %d\n", ret);
		return ret;
	}

	cpu_input_boost_input_handler.private = b;
	ret = input_register_handler(&cpu_input_boost_input_handler);
	if (ret) {
		pr_debug("Failed to register input handler, err: %d\n", ret);
		goto unregister_cpu_notif;
	}

	b->msm_drm_notif.notifier_call = msm_drm_notifier_cb;
	b->msm_drm_notif.priority = INT_MAX;
	ret = msm_drm_register_client(&b->msm_drm_notif);
	if (ret) {
		pr_debug("Failed to register msm_drm notifier, err: %d\n", ret);
		goto unregister_handler;
	}

	thread = kthread_run(cpu_boost_thread, b, "cpu_boostd");
	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
		pr_debug("Failed to start CPU boost thread, err: %d\n", ret);
		goto unregister_fb_notif;
	}

	return 0;

unregister_fb_notif:
	msm_drm_unregister_client(&b->msm_drm_notif);
unregister_handler:
	input_unregister_handler(&cpu_input_boost_input_handler);
unregister_cpu_notif:
	cpufreq_unregister_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	return ret;
}
subsys_initcall(cpu_input_boost_init);
