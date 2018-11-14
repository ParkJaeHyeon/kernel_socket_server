#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/errno.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/netdevice.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/delay.h>
#include <linux/un.h>
#include <linux/unistd.h>
#include <linux/wait.h>
#include <linux/ctype.h>
#include <asm/unistd.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <net/inet_connection_sock.h>
#include <net/request_sock.h>
#include <linux/version.h>

#define DEFAULT_PORT 2325
#define Connect_PORT 23
#define MODULE_NAME "ktcp"
#define INADDR_SEND INADDR_LOPPBACK

static atomic_t revc_count;
static atomic_t send_count;

struct ktcp_service
{
	int running;
	raw_spinlock_t mLock;
	struct socket *listen_socket;
	struct task_struct *thread;
	struct task_struct *accept_worker;
};
struct ktcp_service *ktcp_svc;

int ktcp_recv(struct socket *sock,unsigned char *buf,int len)
{
	if(sock == NULL)
	{
		printk("krecv the csock is NULL\n");
		return -1;
	}	
	else
	{
		//printk("krecv.the csock is:%x, %x\n", (int)sock,ktcp_svc->listen_socket);
	}
	//printk("Test the csock:%x\n",sock->sk->sk_rmem_alloc);
	printk("ktcp_recv\n");
	struct msghdr msg;
	struct iovec iov;
	
	int size = 0;
	{
		if(sock->sk==NULL)
			return 0;
		iov.iov_base = buf;
		iov.iov_len = len;
		#if LINUX_VERSION_CODE <= KERNEL_VERSION(4,0,0)
			msg.msg_iov = &iov;
			msg.msg_iovlen = 1;
		#else
			iov_iter_init(&msg.msg_iter,WRITE,&iov,1,len);
		#endif

		msg.msg_name= NULL;
		msg.msg_namelen=0;
		msg.msg_control = NULL;
		msg.msg_controllen = 0;
		msg.msg_flags = 0;
	}
	#if LINUX_VERSION_CODE <= KERNEL_VERSION(4,0,0)
		szie = sock_recvmsg(sock,&msg,len,0);
		printk("aa");
	#else
		size = sock_recvmsg(sock,&msg,0);
		printk("bb");
	#endif
		
	printk("ktcp_recved size = %d\n",size);
	printk("the message is : %s\n",buf);
	atomic_inc(&revc_count);
		
	return size;
}
int ktcp_send(struct socket *sock,char *buf, int len)
{
	printk("ktcp_send");
	if(sock==NULL)
	{
		printk("send the csock is NULL\n");
		return -1;
	}
	struct msghdr msg;
	struct iovec iov;
	int size;
	
	iov.iov_base = (void *)buf;
	iov.iov_len = len;

	#if LINUX_VERSION_CODE <= KERNEL_VERSION(4,0,0)
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
	#endif
	
	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags = 0;
	
	#if LINUX_VERSION_CODE <= KERNEL_VERSION(4,0,0)
		size = sock_sendmsg(sock,&msg,len);
	#else
		size = sock_sendmsg(sock,&msg);
	#endif
	printk("message sent!\n");
	
	atomic_inc(&send_count);
	
	return size;
}

int ktcp_accept_worker(void *data)
{
	printk("accept_worker fired!\n");
	int error, ret;
	struct socket *socket;
	struct socket *csock;
	int len=10;
	unsigned char buf[len+1];
	
	//printk("declare the wiat queue in the accept_worker\n");
	DECLARE_WAITQUEUE(wait,current);
	
	raw_spin_lock(&(ktcp_svc->mLock));
	{
		ktcp_svc->running = 1;
		current->flags |= PF_NOFREEZE;
		allow_signal(SIGKILL|SIGSTOP);
	}
	raw_spin_unlock(&(ktcp_svc->mLock));

	socket = ktcp_svc->listen_socket;
	printk("CREATE the client accept socket\n");
	csock=(struct socket*)kmalloc(sizeof(struct socket),GFP_KERNEL);
	error = sock_create(PF_INET,SOCK_STREAM,IPPROTO_TCP,&csock);
	
	if(error < 0)
	{
		printk("CREATE CSOCKET ERROR");
		return error;
	}	
	
	//printk("accept_worker.the cscok is :%x,%x\n",csock,ktcp_svc->listen_socket);
	
	struct inet_connection_sock *isock = inet_csk(socket->sk);
	while(ktcp_svc->running == 1)
	{
		if(reqsk_queue_empty(&isock->icsk_accept_queue))
		{
			add_wait_queue(&socket->sk->sk_wq->wait,&wait);
			__set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ);
			//printk("icsk queue empty?: %d\n",reqsk_queue_empty(&isock->icsk_accept_queue));
			//printk("recv queue empty?: %d\n",skb_queue_empty(&socket->sk->sk_receive_queue));
			
			__set_current_state(TASK_RUNNING);
			remove_wait_queue(&socket->sk->sk_wq->wait,&wait);
			continue;
		}
		printk("do accept\n");
		ret = socket->ops->accept(socket,csock,O_NONBLOCK);
		if(ret<0)
		{
			printk("accept error,release the socket\n");
			sock_release(csock);
			return ret;
		}
		memset(&buf,0,len+1);
		printk("do receive the package\n");
		ret = ktcp_recv(csock,buf,len);
		if(0 <= ret)
		{
			//ret = ktcp_send(csock,buf,strlen(buf));
		}
		
			//memset(&buf,0,len+1);
			//strcat(buf,"testing...");
			//printk("do send the package\n");
			//ktcp_send(csock,buf,strlen(buf));
		
		msleep(10);
	}
	return ret;
}
int ktcp_start_listen(void *data)
{
	printk("ktcp_start_listen()\n");
	int error;
	struct socket *socket;
	struct sockaddr_in sin,sin_send;
	
	DECLARE_WAIT_QUEUE_HEAD(wq);
	raw_spin_lock(&(ktcp_svc->mLock));
	{
		ktcp_svc->running = 1;
		current->flags |= PF_NOFREEZE;
		allow_signal(SIGKILL|SIGSTOP);
	}
	raw_spin_unlock(&(ktcp_svc->mLock));
	
	error = sock_create(PF_INET,SOCK_STREAM,IPPROTO_TCP,&ktcp_svc->listen_socket);
	printk("CREATE SOCKET\n");
	if(error<0)
	{
		printk("CREATE SOCKET ERROR\n");
		return -1;
	}
	socket = ktcp_svc->listen_socket;
	ktcp_svc->listen_socket->sk->sk_reuse=1;

	sin.sin_addr.s_addr = htonl(INADDR_ANY);
	sin.sin_family=AF_INET;
	sin.sin_port=htons(DEFAULT_PORT);
	
	error = socket->ops->bind(socket,(struct sockaddr*)&sin,sizeof(sin));
	printk("SOCKET BIND\n");
	if(error <0)
	{
		printk("BIND ERROR\n");
		return -1;
	}
	error = socket->ops->listen(socket,5);
	printk("SOCKET LISTEN\n");
	if(error<0)
	{
		printk("LISTEN ERROR\n");
		return -1;
	}
	ktcp_svc->accept_worker=kthread_run((void*)ktcp_accept_worker,NULL,MODULE_NAME);
	while(ktcp_svc->running == 1)
	{
		wait_event_timeout(wq,0,3*HZ);
		if(signal_pending(current))
		{
			printk("signal_pending()\n");
			break;
		}
		msleep(10);
	}	
	printk("thread STOP\n");
	return 1;
}	
int ktcp_start(void)
{
	printk("ktcp_start()\n");
	ktcp_svc->running = 0;
	raw_spin_lock_init(&(ktcp_svc->mLock));
	ktcp_svc->thread = kthread_run((void*)ktcp_start_listen, NULL,MODULE_NAME);
	printk("ktcp_stop()\n");
	return 0;
}	
int init_module()
{
	printk("module init\n");
	ktcp_svc = kmalloc(sizeof(struct ktcp_service),GFP_KERNEL);
	ktcp_start();
	return 0;
}

void cleanup_module()
{
	int err;
	printk("module cleanup\n");
	if(ktcp_svc->thread==NULL)
	{
		printk("no kernel thread to kill\n");
	}
	else
	{	
		raw_spin_lock(&(ktcp_svc->mLock));
		{
			ktcp_svc->running = 0;
			printk("stop the thread\n");
			err = kthread_stop(ktcp_svc->thread);
		}
		raw_spin_unlock(&(ktcp_svc->mLock));
	
		if(ktcp_svc->listen_socket != NULL)
		{
			printk("release the listen_socket\n");
			sock_release(ktcp_svc->listen_socket);
			ktcp_svc->listen_socket = NULL;
		}
	
		kfree(ktcp_svc);
		ktcp_svc = NULL;

		printk("module unloaded\n");
	}
}

