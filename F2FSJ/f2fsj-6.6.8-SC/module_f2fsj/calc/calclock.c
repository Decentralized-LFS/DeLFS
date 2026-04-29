/*
 * Per-CPU Calclock implementation
 */

#include "calclock.h"
#include <linux/slab.h>

static const char *separate(char *buf, size_t buf_len)
{
	char *reversed_buf;	/* buffer for storing reversed numbers */
	int cur, counter = 0, rvs_cur = 0;

	reversed_buf = kmalloc(buf_len, GFP_KERNEL);
	cur = strlen(buf);
	for (--cur; cur > -1; cur--) {
		if (counter == 3) {
			reversed_buf[rvs_cur++] = ',';
			counter = 0;
			cur++;
			continue;
		}
		reversed_buf[rvs_cur++] = buf[cur];
		counter++;
	}

	cur = 0;
	for (--rvs_cur; rvs_cur > -1; rvs_cur--)
		buf[cur++] = reversed_buf[rvs_cur];

	kfree(reversed_buf);
	return buf;
}

/**
 * sep_num() - Format number with comma separators
 * @num: Value to format
 * @buf: Buffer to store result
 * @buf_len: Size of @buf
 *
 * Return: @buf containing formatted @num
 */
static inline const char *sep_num(const uint64_t num, char *buf, size_t buf_len)
{
	memset(buf, 0, buf_len);
	sprintf(buf, "%llu", num);
	return separate(buf, buf_len);
}

/**
 * sep_nsec() - Format nsec with comma separators
 * @nsec: Time value to format
 * @buf: Buffer to store result
 * @buf_len: Size of @buf
 *
 * Return: @buf containing formatted @num
 */
static inline const char *sep_nsec(const ktime_t nsec, char *buf, size_t buf_len)
{
	memset(buf, 0, buf_len);
	sprintf(buf, "%lld", nsec);
	return separate(buf, buf_len);
}

void __ktprint(int depth, char *fn_name, ktime_t time,
		uint64_t count, size_t nr_threads)
{
	char buff[100], buff2[100], buff3[100];	// buffer for characterized numbers
	int percentage;
	static ktime_t totaltime = 1;

	if (ktime_before(totaltime, time))
		totaltime = time;
	percentage = time * 10000 / totaltime;

	printk("%s", "");
	while(depth--)
		pr_cont("    ");
	pr_cont("%s is called %s times, and the time interval is %sns "
		"(per thread is %sns w/ %ld threads) (%d.%d%%)\n",
		fn_name, sep_num(count, buff, sizeof(buff)),
		sep_nsec(ktime_to_ns(time), buff2, sizeof(buff2)), 
		sep_nsec(ktime_to_ns(time) / (nr_threads ? : 1), buff3, sizeof(buff3)),
		nr_threads, percentage / 100, percentage % 100);
}
