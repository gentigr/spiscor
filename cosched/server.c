#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

#define	_LISTENQ 10	/*!< Max length for accepted connections */
#define	_PORT 1024	/*!< Port for listening */

//! Macros to log entrying in function
#define LOG_ENTERING() do { \
	handle_message(LOG_DEBUG, "+%s::%s::%d", __FILE__, __FUNCTION__, __LINE__); \
	} while (0);
//! Macros to log exiting from function
#define LOG_EXITING() do { \
	handle_message(LOG_DEBUG, "-%s::%s::%d", __FILE__, __FUNCTION__, __LINE__); \
	} while (0);

struct pool_settings {
	pthread_mutex_t	mutex;	//!< Mutex for threads
	pthread_cond_t	condvar;//!< Condition to release resources
	unsigned int	count;	//!< Count of working threads
	unsigned int	lsocket;//!< Listen socket for threads
};

//! Function-wrapper for syslog messages.
/*! Retransmitt messages into syslog and exiting program if need.
 * \param [in] type - priority of error: LOG_ERR, LOG_WARNING etc
 * \param [in] msg - format error message
 * \param [in] ... - data for format error message
 * \return nothing
 */
void handle_message(int type, const char *msg, ...) {
	va_list args;
	va_start(args, msg);
	char buff[1024];
	if (vsprintf(buff, msg, args) == 0) {
		buff[0] = 0;
	}
	openlog("SPISCOR:transeiver", 0, LOG_USER);
	syslog(type, buff);
	closelog();
	va_end(args);
	if (type <= LOG_ERR) {
		exit( EXIT_FAILURE );
	}
}

//! Function for sending size of message.
/*! Send size of transmitt file.
 * \param [in] size - number for sending
 * \param [in] sd - socket descriptor for sending
 * \return nothing
 */
void send_size_of_message(int size, int sd) {
	int converted_size = htonl(size);
	send(sd, (const char*)&converted_size, 4, 0);
}

//! Function for sending data.
/*! Send file into socket.
 * \param [in] file - name of file
 * \param [in] connfd - descriptor of socket
 * \return 0 if ended without error
 */
int send_file(char *file, int connfd) {
	LOG_ENTERING();
	struct stat st;
	stat(file, &st);
	int size = st.st_size;
	handle_message(LOG_DEBUG, "Size of file '%s' = %d", file, size);
	send_size_of_message(size, connfd);

	char bufer[1024];
	FILE *in = fopen(file, "rb");
	int i = 0;
	while (!feof(in)) {
		int b = fread(bufer, 1, sizeof(bufer), in);
		size = ftell(in);
		handle_message(LOG_DEBUG, "Chunk read: %d, part: %d, pos: %d", b, i, size);
		if (b != 0) {
			send(connfd, bufer, b, 0);
		}
		i++;
	}
	fclose(in);
	LOG_EXITING();
	return 0;
}

//! Create and prepare listening socket.
/*! Initialize listening socket.
 * \param [in] p - port for listening
 * \param [in] addr - struct for socket
 * \return listening socket ls
 */
int getsocket(in_port_t p, struct sockaddr_in *addr) {
	LOG_ENTERING();
	int rc = 1, ls;
	if ((ls = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		handle_message(LOG_ERR, "Create stream socket failed.", 0);
	}
	if (setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &rc, sizeof(rc)) != 0) {
		handle_message(LOG_ERR, "Set socket option failed.", 0);
	}
	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_port = htons(p);
	addr->sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(ls, (struct sockaddr*)addr, sizeof(*addr)) != 0) {
		handle_message(LOG_ERR, "Bind socket address failed.", 0);
	}
	if (listen(ls, _LISTENQ) != 0) {
		handle_message(LOG_ERR, "Put socket in listen state failed.", 0);
	}
	LOG_EXITING();
	return ls;
}

//! Printing pthread id.
/*! Print native address of pthread id.
 * \param [in] addr - address of pthread
 * \param [in] size - length of address
 * \param [out] address - string with result address
 * \return nothing
 */
static void print_raw_pthread_id(void *addr, size_t size, char* address) {
	unsigned char *base = addr;
	size_t i;
	for (i = 0; i < size; i++) {
		sprintf(address++, "%02x", base[i]);
	}
}

//! Worker function for threads.
/*! Listen port and accept connections.
 * \param [in] ps - pool_settings structure with settings
 * \return nothing
 */
void* worker( void* ps ) {
	LOG_ENTERING();

	pthread_t self = pthread_self();
	char address[sizeof(self)];
	print_raw_pthread_id(&self, sizeof(self), address);
	handle_message(LOG_DEBUG, "%s", address);

	struct pool_settings *sc = (struct pool_settings*)ps;
	int rs;
	sched_yield();
	if ((rs = accept(sc->lsocket, NULL, NULL)) < 0) {
		handle_message(LOG_ERR, "Accept error.", 0);
	}
	pthread_mutex_lock(&(sc->mutex));
	sc->count++;
	system("gcc sample_task.c -o sample_task");
	send_file("sample_task", rs);
	send_file("data.txt", rs);
	close(rs);
	system("rm sample_task");
	pthread_cond_signal(&(sc->condvar));
	pthread_mutex_unlock(&(sc->mutex));
	sleep(250);

	LOG_EXITING();
	return NULL;
}

//! The main function.
/*! The entry point of programm.
 * \param [in] argc - number of arguments
 * \param [in] argv - list of arguments
 * \return EXIT_SUCCESS when successfully completed
 */
int main(int argc, char **argv) {
	LOG_ENTERING();

	struct sockaddr_in addr;
	int ls = getsocket(_PORT, &addr);              
	struct pool_settings ps = {	PTHREAD_MUTEX_INITIALIZER,
					PTHREAD_COND_INITIALIZER,
					3,
					ls};
	while (1) {
		pthread_t tid;
		if (pthread_create(&tid, NULL, &worker, &ps) != 0) {
			handle_message(LOG_ERR, "Thread create error.");
		}
		sched_yield();      
		pthread_mutex_lock(&(ps.mutex));
		ps.count--;
		while (ps.count <= 0) {
			pthread_cond_wait(&(ps.condvar), &(ps.mutex));
		}
		pthread_mutex_unlock(&(ps.mutex));
	};
	close(ls);

	LOG_EXITING();
	exit(EXIT_SUCCESS);
};
