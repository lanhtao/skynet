#include "skynet_timer.h"
#include "skynet_mq.h"
#include "skynet_server.h"
#include "skynet_handle.h"
#include "skynet.h"

#include <time.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#if defined(__APPLE__)
#include <sys/time.h>
#endif
//定时器：http://www.ibm.com/developerworks/cn/linux/l-cn-timers/
typedef void (*timer_execute_func)(void *ud,void *arg);

#define TIME_NEAR_SHIFT 8
#define TIME_NEAR (1 << TIME_NEAR_SHIFT)
#define TIME_LEVEL_SHIFT 6
#define TIME_LEVEL (1 << TIME_LEVEL_SHIFT)
#define TIME_NEAR_MASK (TIME_NEAR-1)
#define TIME_LEVEL_MASK (TIME_LEVEL-1)

struct timer_event {
	uint32_t handle;
	int session;//标记这个定时间时间。作为查询的依据
};

struct timer_node {
	struct timer_node *next;
	int expire;//过期时间。当timer结构体中的time>=expire时，执行定时器回调函数
};

struct link_list {
	struct timer_node head;
	struct timer_node *tail;
};

struct timer {
	struct link_list near[TIME_NEAR];//定时器容器组 存放了不同的定时器容器
	struct link_list t[4][TIME_LEVEL-1];//4级梯队 4级不同的定时器
	int lock;//原子操作锁
	int time;//当前已经走过的时间
	uint32_t current;//当前时间，绝对时间
	uint32_t starttime;//相对系统开机的时间
};

//申明一个全局的定时器
static struct timer * TI = NULL;

//清理定时器链表，并返回原链表第一个节点指针
static inline struct timer_node *
link_clear(struct link_list *list)
{
	struct timer_node * ret = list->head.next;
	list->head.next = 0;
	list->tail = &(list->head);

	return ret;
}

// 将node添加到链表尾部
static inline void
link(struct link_list *list,struct timer_node *node)
{
	list->tail->next = node;
	list->tail = node;
	node->next=0;
}

// 对于内核最关心的、interval值在［0，255］
// 内核在处理是否有到期定时器时，它就只从定时器向量数组tv1.vec［256］中的某个定时器向量内进行扫描。
// （2）而对于内核不关心的、interval值在［0xff，0xffffffff］之间的定时器，
// 它们的到期紧迫程度也随其interval值的不同而不同。显然interval值越小，定时器紧迫程度也越高。
// 因此在将它们以松散定时器向量进行组织时也应该区别对待。通常，定时器的interval值越小，
// 它所处的定时器向量的松散度也就越低（也即向量中的各定时器的expires值相差越小）；而interval值越大，
// 它所处的定时器向量的松散度也就越大（也即向量中的各定时器的expires值相差越大）。

// 内核规定，对于那些满足条件：0x100≤interval≤0x3fff的定时器，
// 只要表达式（interval>>8）具有相同值的定时器都将被组织在同一个松散定时器向量中，
// 即以1》8＝256为一个基本单位。因此，为组织所有满足条件0x100≤interval≤0x3fff的定时器，
// 就需要2^6＝64个松散定时器向量。同样地，为方便起见，这64个松散定时器向量也放在一起形成数组，并作为数据结构timer_vec的一部分。
static void
add_node(struct timer *T,struct timer_node *node)
{
	int time=node->expire;//定时器超时的时间
	int current_time=T->time;//当前走过的时间
	
	//判断超时时间和当前走过的时间是否足够接近，进行分等级
	if ((time|TIME_NEAR_MASK)==(current_time|TIME_NEAR_MASK)) {
		//超时时间小于256
		link(&T->near[time&TIME_NEAR_MASK],node);
	}
	else {
		//随着下标变大，超时时间增大一个数量级
		int i;
		int mask=TIME_NEAR << TIME_LEVEL_SHIFT;
		for (i=0;i<3;i++) {
			if ((time|(mask-1))==(current_time|(mask-1))) {
				break;
			}
			mask <<= TIME_LEVEL_SHIFT;
		}
		link(&T->t[i][((time>>(TIME_NEAR_SHIFT + i*TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK)-1],node);	
	}
}

//向定时器T的链表中增加一个超时时间
static void
timer_add(struct timer *T,void *arg,size_t sz,int time)
{
	struct timer_node *node = (struct timer_node *)malloc(sizeof(*node)+sz);
	memcpy(node+1,arg,sz);//在node紧随其后的地址中存放arg参数

	while (__sync_lock_test_and_set(&T->lock,1)) {};

		node->expire=time+T->time;//设置time时间后定时时间到
		add_node(T,node);

	__sync_lock_release(&T->lock);
}

static void
timer_shift(struct timer *T) {
	int mask = TIME_NEAR;//256
	int time = (++T->time) >> TIME_NEAR_SHIFT;//定时时间加1
	int i=0;
	
	while ((T->time & (mask-1))==0) {//当前过去的时间小于256
		int idx=time & TIME_LEVEL_MASK;//==0：time大于63；!=0:小于63
		if (idx!=0) {
			--idx;
			struct timer_node *current = link_clear(&T->t[i][idx]);//获取链表中的某个定时器的头指针，并情况链表
			while (current) {//将该链表重新添加到定时器中。其等级将偏移
				struct timer_node *temp=current->next;
				add_node(T,current);
				current=temp;
			}
			break;				
		}
		mask <<= TIME_LEVEL_SHIFT;
		time >>= TIME_LEVEL_SHIFT;
		++i;
	}	
}

static inline void
timer_execute(struct timer *T) {
	int idx = T->time & TIME_NEAR_MASK;//==0:大于255；!=0：小于255
	if(idx != 0)
	{
		while (T->near[idx].head.next) {
			struct timer_node *current = link_clear(&T->near[idx]);//获取超时时间time的链表首指针
			
			do {
				struct timer_event * event = (struct timer_event *)(current+1);//对应着timer_add函数中的加1；
				struct skynet_message message;
				message.source = 0;
				message.session = event->session;
				message.data = NULL;
				message.sz = PTYPE_RESPONSE << HANDLE_REMOTE_SHIFT;
	
				skynet_context_push(event->handle, &message);
				
				struct timer_node * temp = current;
				current=current->next;
				free(temp);	
			} while (current);
		}
	}
}

static void 
timer_update(struct timer *T)
{
	while (__sync_lock_test_and_set(&T->lock,1)) {};

	// try to dispatch timeout 0 (rare condition)
	//极少执行的超时
	timer_execute(T);

	// shift time first, and then dispatch timer message
	timer_shift(T);
	timer_execute(T);

	__sync_lock_release(&T->lock);
}

static struct timer *
timer_create_timer()
{
	struct timer *r=(struct timer *)malloc(sizeof(struct timer));
	memset(r,0,sizeof(*r));

	int i,j;

	for (i=0;i<TIME_NEAR;i++) {
		link_clear(&r->near[i]);
	}

	for (i=0;i<4;i++) {
		for (j=0;j<TIME_LEVEL-1;j++) {
			link_clear(&r->t[i][j]);
		}
	}

	r->lock = 0;
	r->current = 0;

	return r;
}

int
skynet_timeout(uint32_t handle, int time, int session) {
	if (time == 0) {
		struct skynet_message message;
		message.source = 0;
		message.session = session;
		message.data = NULL;
		message.sz = PTYPE_RESPONSE << HANDLE_REMOTE_SHIFT;

		if (skynet_context_push(handle, &message)) {
			return -1;
		}
	} else {
		struct timer_event event;
		event.handle = handle;
		event.session = session;
		timer_add(TI, &event, sizeof(event), time);
	}

	return session;
}

/*
**获得当前时间点的时间，单位为0.01s
**返回值为多少个厘秒
*/
static uint32_t
_gettime(void) {
	uint32_t t;
#if !defined(__APPLE__)
	struct timespec ti;
	clock_gettime(CLOCK_MONOTONIC, &ti);
	t = (uint32_t)(ti.tv_sec & 0xffffff) * 100;
	t += ti.tv_nsec / 10000000;
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	t = (uint32_t)(tv.tv_sec & 0xffffff) * 100;
	t += tv.tv_usec / 10000;
#endif
	return t;
}

void
skynet_updatetime(void) {
	uint32_t ct = _gettime();
	if (ct != TI->current) {
		int diff = ct>=TI->current?ct-TI->current:(0xffffff+1)*100-TI->current+ct;
		TI->current = ct;
		int i;
		for (i=0;i<diff;i++) {
			timer_update(TI);
		}
	}
}

uint32_t
skynet_gettime_fixsec(void) {
	return TI->starttime;
}

uint32_t 
skynet_gettime(void) {
	return TI->current;
}

void 
skynet_timer_init(void) {
	TI = timer_create_timer();//创建全局的定时器，并清空定时器链表
	TI->current = _gettime();//获取当前时间，单位为厘秒

#if !defined(__APPLE__)
	struct timespec ti;
	clock_gettime(CLOCK_REALTIME, &ti);
	uint32_t sec = (uint32_t)ti.tv_sec;
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	uint32_t sec = (uint32_t)tv.tv_sec;//获取当前时间，未计算单位为微秒的数，单位为秒
#endif
	uint32_t mono = _gettime() / 100;//获取当前时间，未计算单位小于秒的数，单位为秒

	TI->starttime = sec - mono;//相对时间。
}
