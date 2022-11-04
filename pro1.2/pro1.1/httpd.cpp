#include <iostream>

#include "httpd.h"

using namespace std;

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <sys/wait.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <set>
#include "threadpool.h"

// #define PORT          8080     /* Server port */
#define HEADER_SIZE   10240L /* Maximum size of a request header line */
#define CGI_POST      10240L /* Buffer size for reading POST data to CGI */
#define CGI_BUFFER    10240L /* Buffer size for reading CGI output */
#define FLAT_BUFFER   10240L /* Buffer size for reading flat files */

/*
 * Standard extensions
 */
#define USE_IP_DENY 0
#define ENABLE_EXTENSIONS
#ifdef  ENABLE_EXTENSIONS
#define ENABLE_CGI      1    /* Whether or not to enable CGI (also POST and HEAD) */
#define ENABLE_DEFAULTS 1    /* Whether or not to enable default index files (.php, .pl, .html) */
#else
#define ENABLE_CGI      0
#define ENABLE_DEFAULTS 0
#endif

/*
 * Default indexes and execution restrictions.
 */
#define INDEX_DEFAULTS  {(char*)"index.php", (char*)"index.pl", (char*)"index.py", (char*)"index.htm", (char*)"index.html", 0}
#define INDEX_EXECUTES  {          1,          1,          1,           0,            0, (unsigned int)-1}

/*
 * Directory to serve out of.
 */
// #define PAGES_DIRECTORY "htdocs"
#define VERSION_STRING  "klange/0.5"

/*
 * Incoming request socket data
 */
struct socket_request {
	int                fd;       /* 文件描述符（客户端） */
	socklen_t          addr_len; /* Length of the address type */
	struct sockaddr_in address;  /* Remote address */
	pthread_t          thread;   /* 处理客户端的线程 */
};

/*
 * CGI process data
 */
struct cgi_wait {
	int                fd;       
	int                fd2;      
	int                pid;      /* Process ID */
};

/*
 * Server socket.
 */
int serversock;

/*
 * Port
 */
int port;
// char* docs;
string docs;

//threadpool_t *thp=NULL; 

/*
 * Last unaccepted socket pointer
 * so we can free it.
 */
void * _last_unaccepted;

/*
 * Better safe than sorry,
 * shutdown the socket and exit.
 */
void handleShutdown(int sig) {
	printf("\n[info] Shutting down.\n");

	/*
	 * Shutdown the socket.
	 */
	shutdown(serversock, SHUT_RDWR);
	close(serversock);

	/*
	 * Free the thread data block
	 * for the next expected connection.
	 */
	free(_last_unaccepted);

	/*
	 * Exit.
	 */
	exit(sig);
}

/*
 * Resizeable vector
 */
typedef struct {
	void ** buffer;
	unsigned int size;
	unsigned int alloc_size;
} vector_t;

#define INIT_VEC_SIZE 1024

vector_t * alloc_vector(void) {
	vector_t* v = (vector_t *) malloc(sizeof(vector_t));
	v->buffer = (void **) malloc(INIT_VEC_SIZE * sizeof(void *));
	v->size = 0;
	v->alloc_size = INIT_VEC_SIZE;

	return v;
}

void free_vector(vector_t* v) {
	free(v->buffer);
	free(v);
}

void vector_append(vector_t * v, void * item) {
	if(v->size == v->alloc_size) {
		v->alloc_size = v->alloc_size * 2;
		v->buffer = (void **) realloc(v->buffer, v->alloc_size * sizeof(void *));
	}

	v->buffer[v->size] = item;
	v->size++;
}

void * vector_at(vector_t * v, unsigned int idx) {
	return idx >= v->size ? NULL : v->buffer[idx];
}

/*
 * Delete a vector
 * Free its contents and then it.
 */
void delete_vector(vector_t * vector) {
	unsigned int i = 0;
	for (i = 0; i < vector->size; ++i) {
		free(vector_at(vector, i));
	}
	free_vector(vector);
}

/*
 * 将两个十六进制的数字转换为ACSII码
 */
char from_hex(char ch) {
	//看看是数字或者是字符
	return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

/*
 * 请求错误的处理集合400，403，500
 */
void generic_response(FILE * socket_stream, char * status, char * message) {
	fprintf(socket_stream,
			"HTTP/1.1 %s\r\n"
			"Server: " VERSION_STRING "\r\n"
			"Content-Type:text/html; charset=utf-8\r\n"
			"\r\n"
			"%s ERROR HAPPEN!!! %s\r\n", status, status, message);
}

/*
 * Wait for a CGI thread to finish and
 * close its pipe.
 */
void *wait_pid(void * onwhat) {
	struct cgi_wait * cgi_w = (struct cgi_wait*)onwhat;
	int status;

	/*
	 * Wait for the process to finish
	 */
	waitpid(cgi_w->pid, &status, 0);

	/*
	 * Close the respective pipe
	 */
	close(cgi_w->fd);
	close(cgi_w->fd2);

	/*
	 * Free the data we were sent.
	 */
	free(onwhat);
	return NULL;
}

int ip_deny(){
	set<string> deny;
    char buf[256];
    FILE *p = fopen("htdocs/.htaccess", "r");
    char temp[1024];
    fgets(temp,sizeof(temp),p);
    while(!feof(p))
    {
        printf("%s",temp);
        if(temp[0]=='d'){
            char *str = temp+10;
            int len = strlen(str);
            str[len-1]='\0';
            printf("%s",str);
            deny.insert(str);
            cout<<strcmp(str,"0.0.0.0");
        }
        fgets(temp,sizeof(temp),p);
    }
    fclose(p);
    set<string>::iterator iter;
     if((iter = deny.find("0.0.0.0")) != deny.end())
     {
         return 1;
     }
	return 0;
}

/*
 * Handle an incoming connection request.
 */
void *handleRequest(void *socket) {
	struct socket_request * request = (struct socket_request *)socket;
	
	/*
	 * Convert the socket into a standard file descriptor
	 */
	FILE *socket_stream = NULL;
	socket_stream = fdopen(request->fd, "r+");
	//文件读取失败
	if (!socket_stream) {
		fprintf(stderr,"Ran out of a file descriptors, can not respond to request.\n");
		goto _disconnect;
	};
	//是否启动ip拒绝
	if(USE_IP_DENY){
		if(ip_deny){
		    generic_response(socket_stream, (char *)"403 forbidden", (char *)"IP forbbien: your ip can not visit.");
			goto _disconnect;
	    }
	}
	

	/*
	 * Read requests until the client disconnects.
	 */
	while (1) {
		vector_t * queue = alloc_vector();
		char buf[HEADER_SIZE];
		//将请求首部和请求头部读到queu里面
		while (!feof(socket_stream)) {
			/*
			 * While the client has not yet disconnected,
			 * read request headers into the queue.
			 */
			char * in = fgets( buf, HEADER_SIZE - 2, socket_stream );
			//1 读取出来是null
			if (!in) {
				/*
				 * EOF
				 */
				break;
			}
		    //2 读取出来是请求头的末尾,也就是多出来的那个\r\n,字符串比较函数相同返回0
			if (!strcmp(in, "\r\n") || !strcmp(in,"\n")) {
				/*
				 * Reached end of headers.
				 */
				break;
			}
			//3 strstr()函数用于找到子串在一个字符串中第一次出现的位置，找不到说明是超大的请求头
			if (!strstr(in, "\n")) {
				/*
				 * Oversized request line.
				 */
				generic_response(socket_stream, (char *)"400 Bad Request", (char *)"Bad request: Request line was too long.");
				delete_vector(queue);
				goto _disconnect;
			}
			/*
			 * 将请求行存储在此请求的队列中.
			 */
			char * request_line = (char*)malloc((strlen(buf)+1) * sizeof(char));
			strcpy(request_line, buf);
			vector_append(queue, (void*)request_line);
		}

		if (feof(socket_stream)) {
			/*
			 * End of stream -> Client closed connection.
			 */
			delete_vector(queue);
			break;
		}

		/*
		 * Request variables
		 */
		char * filename          = NULL; /* Filename as received (ie, /index.php) */
		char * querystring       = NULL; /* Query string, URL encoded */
		int request_type         = 0;    /* Request type, 0=GET, 1=POST, 2=HEAD ... */
		char * _filename         = NULL; /* Filename relative to server (ie, pages/index.php) */
		char * ext               = NULL; /* Extension for requested file */
		char * host              = NULL; /* Hostname for request, if supplied. */
		char * http_version      = NULL; /* HTTP version used in request */
		unsigned long c_length   = 0L;   /* Content-Length, usually for POST */
		char * c_type            = NULL; /* Content-Type, usually for POST */
		char * c_cookie          = NULL; /* HTTP_COOKIE */
		char * c_uagent          = NULL; /* User-Agent, for CGI */
		char * c_referer         = NULL; /* Referer, for CGI */

		/*
		 * ---------------解析请求头和请求首的循环-------------------------------------------------
		 */
		unsigned int i = 0;
		for (i = 0; i < queue->size; ++i) {
			char * str = (char*)(vector_at(queue,i));

			/*
			 * Find the colon for a header
			 */
			char * colon = strstr(str,": ");
			//找不到:符号
			if (!colon) {
				//找不到:符号且不是请求首部
				if (i > 0) {
					/*
					 * Request string outside of first entry.
					 */
					generic_response(socket_stream,(char *)"400 Bad Request", (char *)"Bad request: A header line was missing colon.");
					delete_vector(queue);
					goto _disconnect;
				}

				/*
				 * 找不到冒号，但是首行: 需要解析请求的类型
				 */
				int r_type_width = 0;
				//从第一个字符开始寻找判断是什么请求
				switch (str[0]) {
					case 'G':
					    //表示所请求是GET 判断是看看找到"GET"的位置是不是首部字符串的起始位置
						if (strstr(str, "GET ") == str) {
							/*
							 * GET: Retreive file
							 */
							r_type_width = 4;
							request_type = 1;
						} else {
							goto _unsupported;
						}
						break;
#if ENABLE_CGI
					case 'P':
						if (strstr(str, "POST ") == str) {
							/*
							 * POST: Send data to CGI
							 */
							r_type_width = 5;
							request_type = 2;
						} else {
							goto _unsupported;
						}
						break;
					case 'H':
						if (strstr(str, "HEAD ") == str) {
							/*
							 * HEAD: Retreive headers only
							 */
							r_type_width = 5;
							request_type = 3;
						} else {
							goto _unsupported;
						}
						break;
#endif
					default:
						/*
						 * Unsupported method.
						 */
						goto _unsupported;
						break;
				}
				//文件名为请求的地址+r_type_width,因为有一个空格(后面还有协议号等待处理)
				filename = str + r_type_width;
				//文件的开头不对的情况
				if (filename[0] == ' ' || filename[0] == '\r' || filename[0] == '\n') {
					/*
					 * Request was missing a filename or was in a form we don't want to handle.
					 */
					generic_response(socket_stream, (char *)"400 Bad Request", (char *)"Bad request: No filename.");
					delete_vector(queue);
					goto _disconnect;
				}

				/*
				 * 获得HTTP版本
				 */
				//找到上一步filename里面开头是HTTP的位置
				http_version = strstr(filename, "HTTP/");
				if (!http_version) {
					/*
					 * No HTTP version was present in the request.
					 */
					generic_response(socket_stream, (char *)"400 Bad Request", (char *)"Bad request: No HTTP version supplied.");
					delete_vector(queue);
					goto _disconnect;
				}
				//设置filename结束符为\0
				http_version[-1] = '\0';
				char * tmp_newline;
				tmp_newline = strstr(http_version, "\r\n");
				if (tmp_newline) {
					tmp_newline[0] = '\0';
				}
				tmp_newline = strstr(http_version, "\n");
				if (tmp_newline) {
					tmp_newline[0] = '\0';
				}
				/*
				 * 获得请求的字符串也就是?后面的内容（GET）
				 */
				querystring = strstr(filename, "?");
				if (querystring) {
					querystring++;
					querystring[-1] = '\0';
				}
			} else {//找到了冒号的情况
				//如果在请求首行有冒号,那就很有问题
				if (i == 0) {
					/*
					 * Non-request line on first line.
					 */
					generic_response(socket_stream, (char *)"400 Bad Request", (char *)"Bad request: First line was not a request.");
					delete_vector(queue);
					goto _disconnect;
				}

				/*
				 * 拆分头部key:value
				 */
				//将冒号的替换成\0
				colon[0] = '\0';
				//将colon定位到value的位置
				colon += 2;
				//将末尾方式\0
				char * eol = strstr(colon,"\r");
				if (eol) {
					eol[0] = '\0';
					eol[1] = '\0';
				} else {
					eol = strstr(colon,"\n");
					if (eol) {
						eol[0] = '\0';
					}
				}

				/*
				 * 处理key 冒号已经被替换成了\0
				 * str: colon
				 */
				//依次读取并存到变量中
				if (!strcmp(str, "Host")) {
					/*
					 * Host: The hostname of the (virtual) host the request was for.
					 */
					host = colon;
					cout<<"主机:"<<host<<endl;
				} else if (!strcmp(str, "Content-Length")) {
					/*
					 * Content-Length: Length of message (after these headers) in bytes.
					 */
					c_length = atol(colon);
				} else if (!strcmp(str, "Content-Type")) {
					/*
					 * Content-Type: MIME-type of the message.
					 */
					c_type = colon;
				} else if (!strcmp(str, "Cookie")) {
					/*
					 * Cookie: CGI cookies
					 */
					c_cookie = colon;
				} else if (!strcmp(str, "User-Agent")) {
					/*
					 * Client user-agent string
					 */
					c_uagent = colon;
				} else if (!strcmp(str, "Referer")) {
					/*
					 * Referer page
					 */
					c_referer = colon;
				}
			}
		}//------------------------------for解析请求报文结束-------------------------------
         
		/*
		 * 所有的请求头和首部已经被读取并记录
		 */
		if (!request_type) {
_unsupported:
			/*
			 * 读不懂请求类型
			 */
			generic_response(socket_stream, (char *)"501 Not Implemented", (char *)"Not implemented: The request type sent is not understood by the server.");
			delete_vector(queue);
			goto _disconnect;
		}
		//文件名空或者有'或者有空格或者在？后面有空格
		if (!filename || strstr(filename, "'") || strstr(filename," ") ||
			(querystring && strstr(querystring," "))) {
			/*
			 * If a filename wasn't specified, we received
			 * an invalid or malformed request and we should
			 * probably dump it.
			 */
			generic_response(socket_stream, (char *)"400 Bad Request", (char *)"Bad request: No filename provided.");
			delete_vector(queue);
			goto _disconnect;
		}

		/*
		 * Get some important information on the requested file
		 * _filename: the local file name, relative to `.`
		 */
		//整合出一个新的文件路径htdoc+filename
		_filename = (char*)calloc(sizeof(char) * (strlen(docs.c_str()) + strlen(filename) + 2), 1);
		strcat(_filename, docs.c_str());
		strcat(_filename, filename);
		//解析URL中的%字符例如
		//%20 : space
		// %21 : !
		// %22 : “
		// %23 : #
		// %24 : $
		// %25 : %
		// %26 : &
		// %27 : ‘
		// %28 : (
		if (strstr(_filename, "%")) {
			/*
			 * Convert from URL encoded string.
			 */
			char * buf = (char*)malloc(strlen(_filename) + 1);//复制一个filename_的空间
			char * pstr = _filename;  //pstr指向开头
			char * pbuf = buf;  //指向新创的另一个filename_的开头
			while (*pstr) {   //没有到filename_的\0
				if (*pstr == '%') {  //出现了%号
					if (pstr[1] && pstr[2]) {  //后面两个字符存在
						*pbuf++ = from_hex(pstr[1]) << 4 | from_hex(pstr[2]);
						pstr += 2;
					}
				} else if (*pstr == '+') {  //+号转义成为空行
					*pbuf++ = ' ';
				} else {
					*pbuf++ = *pstr;
				}
				pstr++;
			}
			*pbuf = '\0';
			free(_filename);//清空指针
			_filename = buf;
		}

		/*
		 * 防止请求非法读取别的文件！！！！！！！！！
		 */
		if (strstr(_filename, "/../") || (strstr(_filename, "/..") == _filename + strlen(_filename) - 3)) {
			generic_response(socket_stream, (char *)"400 Bad Request", (char *)"Bad request");
			free(_filename);
			delete_vector(queue);
			goto _disconnect;
		}

		/*
		 * ext: the file extension, or NULL if it lacks one
		 */
		ext = filename + 1;//第一个位置不可能是. 文件总要有名字
		//循环跳到最后一个.
		// char * tmp = ext;
		// while (strstr(tmp,".")) {
		// 	ext = strstr(tmp,".");
		// 	tmp++;
		// }
		while (strstr(ext+1,".")) {
			ext = strstr(ext+1,".");
		}
		//没有扩展名的情况
		if (ext == filename + 1) {
			/*
			 * Either we didn't find a dot,
			 * or that dot is at the front.
			 * If the dot is at the front, it is not an extension,
			 * but rather an extension-less hidden file.
			 */
			ext = NULL;
		}

		/*
		 * 检查是否是一个文件夹或者是文件
		 */
		struct stat stats;
		//请求的是一个 合法的 文件夹但是没有/符号,表示文件已经被移动
		if (stat(_filename, &stats) == 0 && S_ISDIR(stats.st_mode)) {
			if (_filename[strlen(_filename)-1] != '/') {
				/*
				 * Request for a directory without a trailing /.
				 * Throw a 'moved permanently' and redirect the client
				 * to the directory /with/ the /.
				 */
				fprintf(socket_stream, "HTTP/1.1 301 Moved Permanently\r\n");
				fprintf(socket_stream, "Server: " VERSION_STRING "\r\n");
				fprintf(socket_stream, "Location: %s/\r\n", filename);
				fprintf(socket_stream, "Content-Length: 0\r\n\r\n");
			} else { //--------------------------------请求的是一个文件夹---------------------------------------------------

#if ENABLE_DEFAULTS
				/*
				 * 检查默认页面.
				 */
				struct stat extra_stats;
				char* index_php=NULL;
				index_php=(char*)malloc(strlen(_filename) + 30);

				/*
				 * The types and exection properties of index files
				 * are describe in a #define at the top of this file.
				 */
// #define INDEX_DEFAULTS{(char*)"index.php", (char*)"index.pl", (char*)"index.py", (char*)"index.htm", (char*)"index.html", 0}
// #define INDEX_EXECUTES{          1,          1,          1,           0,            0, (unsigned int)-1}

				char *       index_defaults[] = INDEX_DEFAULTS;  //默认页面的格式
				unsigned int index_executes[] = INDEX_EXECUTES;  //对应的索引
				unsigned int index = 0;

				while (index_defaults[index] != (char *)0) { //遍历index
					index_php[0] = '\0';
					strcat(index_php, _filename);
					strcat(index_php, index_defaults[index]);
					//获取文件信息成功&&S_IXOTH = 0001(16进制)      （其他-执行）//没有权限不可执行！！！！！！！！
					if ((stat(index_php, &extra_stats) == 0) && ((extra_stats.st_mode & S_IXOTH) == index_executes[index])) {
						/*
						 * This index exists, use it instead of the directory listing.
						 */
						_filename = (char*)realloc(_filename, strlen(index_php)+1);
						stats = extra_stats;
						memcpy(_filename, index_php, strlen(index_php)+1);
						ext = _filename;
						while (strstr(ext+1,".")) {
							ext = strstr(ext+1,".");
						}
						//跳转到use_file
						goto _use_file;
					}
					++index;
				}
#endif

				/*
				 * 目录下没有index页面,因此列出该目录下的文件，并且可以点击访问LINK
				 */
				struct dirent **files = {0};  //结构体数组
				int filecount = -1;
				//扫描_filename指定的目录文件到files
				filecount = scandir(_filename, &files, 0, alphasort);

				/*
				 *准备打印的首部和body
				 */
				fprintf(socket_stream, "HTTP/1.1 200 OK\r\n");
				fprintf(socket_stream, "Server: " VERSION_STRING "\r\n");
				fprintf(socket_stream, "Content-Type: text/html\r\n");

				/*
				 * Allocate some memory for the HTML
				 */
				char * listing = (char*)malloc(1024);
				listing[0] = '\0';
				strcat(listing, "<!doctype html><html><head><title>Directory Listing</title></head><body>");
				int i = 0;
				//遍历文件
				for (i = 0; i < filecount; ++i) {
					/*
					 *拼接目录和文件名组成完整的文件名
					 */
					char * _fullname=(char*)malloc(strlen(_filename) + 1 + strlen(files[i]->d_name) + 1);
					sprintf(_fullname, "%s/%s", _filename, files[i]->d_name);
					if (stat(_fullname, &stats) == 0 && S_ISDIR(stats.st_mode)) {//如果是文件夹
						/*
						 * 文件夹就不管了不打印了！！！！！！！
						 */
						free(files[i]);
						continue;
					}

					/*
					 * 为文件添加超链接
					 */
					char *_file=(char*)malloc(2 * strlen(files[i]->d_name) + 64);
					sprintf(_file, "<a href=\"%s\">%s</a><br>\n", files[i]->d_name, files[i]->d_name);
					//新增地址空间
					listing = (char*)realloc(listing, strlen(listing) + strlen(_file) + 1);
					//添加超链接到页面
					strcat(listing, _file);
					//释放空间
					free(files[i]);
				}
				free(files);

				/*
				 * Close up our HTML
				 */
				listing = (char*)realloc(listing, strlen(listing) + 64);
				//增加结束的行,追加后自动不上\0,因此在strlen的时候可以算出正确的返回大小
				strcat(listing,"</body></html>");

				/*
				 * 发送给客户端,%zu表示无符号的整形
				 */
				fprintf(socket_stream, "Content-Length: %zu\r\n", (sizeof(char) * strlen(listing)));
				fprintf(socket_stream, "\r\n");
				fprintf(socket_stream, "%s", listing);
				free(listing);
			}
		} else { //是一个文件,但不一定合法
_use_file:
			;
			/*
			 * 打开请求的文件
			 */
			FILE * content = fopen(_filename, "rb");
			if (!content) {//文件打开失败
				//判断是不是没有可读权限
				struct stat con;
				string page;
				int flag;
				if ((stat(_filename, &con) == 0) && ((con.st_mode & S_IXOTH) == 0)){
					page = "/403.html";
					flag = 1;
				}else{
					page = "/404.html";
					flag = 0;
				}
				/*
				 * Could not open file - 404. (Perhaps 403)
				 */
				string pa = docs+page;
				//打开403或者404页面
				content = fopen(pa.c_str(), "rb");
 
				if (!content) {
					/*
					 * 连403和404页面都没！！！！
					 */
					generic_response(socket_stream, (char *)"404 File Not Found", (char *)"The requested file could not be found.");
					goto _next;
				}

				/*
				 * Replace the internal filenames with the 404 page
				 * and continue to load it.
				 */
				if(flag==0){
					fprintf(socket_stream, "HTTP/1.1 404 File Not Found\r\n");
				}else fprintf(socket_stream, "HTTP/1.1 403 Forbidden\r\n");
				
				_filename = (char*)realloc(_filename, strlen((docs+page).c_str()) + 1);
				//重新分配空间给新的文件名:docs/404.htm
				_filename[0] = '\0';
				strcat(_filename, (docs+page).c_str());
				ext = strstr(_filename, ".");  //重新复制文件后缀ext
			} else {//文件打开成功,也就是文件合法
				/*
				 * We're good to go.
				 */
#if ENABLE_CGI
				if (stats.st_mode & S_IXOTH) {//有可执行权限
					/*
					 * CGI Executable
					 * Close the file
					 */
					fclose(content);

					/*
					 * 创建双管道
					 */
					int cgi_pipe_r[2]; //读管道
					int cgi_pipe_w[2]; //写管道
					if (pipe(cgi_pipe_r) < 0) {
						fprintf(stderr, "Failed to create read pipe!\n");
					}
					if (pipe(cgi_pipe_w) < 0) {
						fprintf(stderr, "Failed to create write pipe!\n");
					}

					/*
					 * fork进程
					 */
					pid_t _pid = 0;
					cout<<"before fork"<<getpid()<<endl;
					_pid = fork(); //创建子进程,通过_pid来区分,等于0的是子进程 
					printf("_pid的值是nm的%d\n",_pid);
					//fork处于哪个线程中，fork后创建的子进程将以该线程作为自己的主线程，并且执行该线程之后的代码
					//在父进程中，fork返回新创建子进程的进程ID；
					if (_pid == 0) {
						cout<<"after fork"<<getpid()<<endl;
						/*
						 * 设置管道
						 *int pipe(int fd[2]);
						 *参数fd[0]: 读端。
						 *fd[1]: 写端。
						 *返回值:成功: 0
						 *失败:-1 errno
						 */
						dup2(cgi_pipe_r[0],STDIN_FILENO); //读管道的读端->到键盘输入
						dup2(cgi_pipe_w[1],STDOUT_FILENO); //写管道的写端->到屏幕输出
						cout<<"what?";
						/*
						 * This is actually cheating on my pipe.
						 */
						fprintf(stdout, "Expires: -1\r\n");

						/*
						 * Operate in the correct directory.
						 */
						char * dir = _filename;
						while (strstr(_filename,"/")) {
							_filename = strstr(_filename,"/") + 1;
						}
						_filename[-1] = '\0';//在CGI文件之前截断,_filename指向cgi文件的第一个字符
						char docroot[1024];
						// char *getcwd(char *buf,size_t size);
						// 参数说明：getcwd()会将当前工作目录的绝对路径复制到参数buffer所指的内存空间中,参数size为buf的空间大小
						getcwd(docroot, 1023);
						strcat(docroot, ("/"+docs).c_str());
						//改变当前工作目录到dir,刚刚好是cgi文件当前的目录
						chdir(dir);

						/*
						 * Set CGI environment variables.
						 * CONTENT_LENGTH    : POST message length
						 * CONTENT_TYPE      : POST encoding type
						 * DOCUMENT_ROOT     : the root directory
						 * GATEWAY_INTERFACE : The CGI version (CGI/1.1)
						 * HTTP_COOKIE       : Cookies provided by client
						 * HTTP_HOST         : Same as above
						 * HTTP_REFERER      : Referer page.
						 * HTTP_USER_AGENT   : Browser user agent
						 * PATH_TRANSLATED   : On-disk file path
						 * QUERY_STRING      : /file.ext?this_stuff&here
						 * REDIRECT_STATUS   : HTTP status of CGI redirection (PHP)
						 * REMOTE_ADDR       : IP of remote user
						 * REMOTE_HOST       : Hostname of remote user (reverse DNS)
						 * REQUEST_METHOD    : GET, POST, HEAD, etc.
						 * SCRIPT_FILENAME   : Same as PATH_TRANSLATED (PHP, primarily)
						 * SCRIPT_NAME       : Request file path
						 * SERVER_NAME       : Our hostname or Host: header
						 * SERVER_PORT       : TCP host port
						 * SERVER_PROTOCOL   : The HTTP version of the request
						 * SERVER_SOFTWARE   : Our application name and version
						 */
						/* setenv("USER","test",1);
						"USER": 环境变量的名字
						"test": 需要设置环境变量的值
						1:  如果为1，则可以设置成功，覆盖原来的值；如果为0，则不能覆盖。 */
						setenv("SERVER_SOFTWARE", VERSION_STRING, 1);
						if (!host) {  //如果请求的头中没有主机名
							char hostname[1024];
							hostname[1023]='\0';
							gethostname(hostname, 1023);
							setenv("SERVER_NAME", hostname, 1);
							setenv("HTTP_HOST",   hostname, 1);
						} else {//有主机名
							setenv("SERVER_NAME", host, 1);
							setenv("HTTP_HOST",   host, 1);
						}
						setenv("DOCUMENT_ROOT", docroot, 1); //CGI文件所在的目录
						setenv("GATEWAY_INTERFACE", "CGI/1.1", 1); //CGI的接口
						setenv("SERVER_PROTOCOL", http_version, 1); //服务协议
						char port_string[20];
						sprintf(port_string, "%d", port);
						setenv("SERVER_PORT", port_string, 1); //TCP主机名 服务器的ip+端口
						if (request_type == 1) { //GET
							setenv("REQUEST_METHOD", "GET", 1);
						} else if (request_type == 2) {  //POST
							setenv("REQUEST_METHOD", "POST", 1); 
						} else if (request_type == 3) {  //HEAD
							setenv("REQUEST_METHOD", "HEAD", 1);
						}
						if (querystring) {  //GET后面有参数
							if (strlen(querystring)) {
								setenv("QUERY_STRING", querystring, 1); //设置参数的环境变量
							} else {
								setenv("QUERY_STRING", "", 1);
							}
						}
						char *fullpath=(char*)malloc(1024 + strlen(_filename));
                        //fullpath是完整的cgi路径名
						getcwd(fullpath, 1023);
						strcat(fullpath, "/");
						strcat(fullpath, _filename);
						setenv("PATH_TRANSLATED", fullpath, 1);
						setenv("SCRIPT_NAME", filename, 1);
						setenv("SCRIPT_FILENAME", fullpath, 1);
						setenv("REDIRECT_STATUS", "200", 1);
						char c_lengths[100];
						c_lengths[0] = '\0';
						sprintf(c_lengths, "%lu", c_length);//无符号长整形输出到数组中
						setenv("CONTENT_LENGTH", c_lengths, 1);
						if (c_type) { //文件类型
							setenv("CONTENT_TYPE", c_type, 1);
						}
						struct hostent * client;
						//主机名和其他的变量
						client = gethostbyaddr((const char *)&request->address.sin_addr.s_addr,
								sizeof(request->address.sin_addr.s_addr), AF_INET);
						if (client != NULL) {
							setenv("REMOTE_HOST", client->h_name, 1);
						}
						setenv("REMOTE_ADDR", inet_ntoa(request->address.sin_addr), 1);
						if (c_cookie) {
							setenv("HTTP_COOKIE", c_cookie, 1);
						}
						if (c_uagent) {
							setenv("HTTP_USER_AGENT", c_uagent, 1);
						}
						if (c_referer) {
							setenv("HTTP_REFERER", c_referer, 1);
						}
						//cout<<"我想输出但是我被人重定向了"<<endl;
						/*
						 * Execute.
						 */
						char executable[1024];
						executable[0] = '\0';
						sprintf(executable, "./%s", _filename);//保存字符 ./xxx.cgi
						execlp(executable, executable, (char *)NULL); //执行CGI
						cout<<"hello";

						/*
						 * The CGI application failed to execute. ;_;
						 * This is a bad thing.
						 */
						fprintf(stderr,"[warn] Failed to execute CGI script: %s?%s.\n", fullpath, querystring);

						/*
						 * Clean the crap from the original process.
						 */
						delete_vector(queue);
						free(dir);
						free(_last_unaccepted);
						pthread_detach(request->thread);
						free(request);

						/*
						 * Our thread back in the main process should be fine.
						 */
						return NULL; //子线程退出,等待主线程杀死子线程和子进程
					}
					cout<<"after excuse"<<getpid()<<endl;
					

					/*
					 * We are the server thread.
					 * Open a thread to close the other end of the pipe
					 * when the CGI application finishes executing.
					 */
					struct cgi_wait * cgi_w = (cgi_wait *)malloc(sizeof(struct cgi_wait));
					printf("我儿子的_pid%d\n",_pid);
					cgi_w->pid = _pid; //父进程都会有id
					cgi_w->fd  = cgi_pipe_w[1]; //写端
					cgi_w->fd2 = cgi_pipe_r[0]; //读端
					pthread_t _waitthread;
					//threadpool_add_task(thp, wait_pid, (void *)cgi_w);
					pthread_create(&_waitthread, NULL, wait_pid, (void *)(cgi_w)); //创建线程回收子线程/进程,顺便关闭管道
                    cout<<"我儿子已经挂了"<<endl;
					/*
					 * Open our end of the pipes.
					 * We map cgi_pipe for reading the output from the CGI application.
					 * cgi_pipe_post is mapped to the stdin for the CGI application
					 * and we pipe our POST data (if there is any) here.
					 */
					FILE * cgi_pipe = fdopen(cgi_pipe_w[0], "r");
					FILE * cgi_pipe_post = fdopen(cgi_pipe_r[1], "w");

					if (c_length > 0) { //如果POST有请求内容的情况
						/*
						 * Write the POST data to the application.
						 */
						cout<<"post请求";
						size_t total_read = 0;
						char buf[CGI_POST];
						while ((total_read < c_length) && (!feof(socket_stream))) { //读取POST的body
							size_t diff = c_length - total_read; 
							if (diff > CGI_POST) {
								/*
								 * If there's more than our buffer left,
								 * obviously, only read enough for the buffer.
								 */
								diff = CGI_POST;
							}
							size_t read;
							read = fread(buf, 1, diff, socket_stream);
							total_read += read;
							cout<<"post的内容";
							cout<<buf<<endl;
							/*
							 * Write to the CGI pipe
							 */
							fwrite(buf, 1, read, cgi_pipe_post);
						}
					}
					if (cgi_pipe_post) {
						/*
						 * If we need to, close the pipe.
						 */
						fclose(cgi_pipe_post);
					}

					/*
					 * 读取cgi程序的头部
					 */
					char buf[CGI_BUFFER];
					if (!cgi_pipe) { //管道打开失败
						generic_response(socket_stream, (char *)"500 Internal Server Error", (char *)"Failed to execute CGI script.");
						//pthread_detach(_waitthread);
						goto _next;
					}
					fprintf(socket_stream, "HTTP/1.1 200 OK\r\n");
					fprintf(socket_stream, "Server: " VERSION_STRING "\r\n");
					unsigned int j = 0;
					//---------------------------------------------开始读CGI文件的内容--------------------------------------------
					//如果文件结束，则返回非0值，否则返回0
					while (!feof(cgi_pipe)) {
						/*
						 * 一直读CGI的头部直到出现单独的\r\n
						 */
						char * in = fgets(buf, CGI_BUFFER - 2, cgi_pipe);
						//如果是空的话
						if (!in) {
							fprintf(stderr,"[warn] Read nothing [%d on %p %d %d]\n", ferror(cgi_pipe), (void *)cgi_pipe, cgi_pipe_w[1], _pid);
							perror("[warn] Specifically");
							buf[0] = '\0';
							break;
						}
						if (!strcmp(in, "\r\n") || !strcmp(in, "\n")) {
							/*
							 * Done reading headers.
							 */
							buf[0] = '\0';
							break;
						}
						//没有冒号或者太长或者不合法？？？
						if (!strstr(in, ": ") && !strstr(in, "\r\n")) {
							/*
							 * Line was too long or is garbage?
							 */
							fprintf(stderr, "[warn] Garbage trying to read header line from CGI [%zu]\n", strlen(buf));
							break;
						}
						fwrite(in, 1, strlen(in), socket_stream); //写回客户端
						++j;
					}
					//读完之后要初始化buf
					if (j < 1) {//CGI没有给头
						fprintf(stderr,"[warn] CGI script did not give us headers.\n");
					}
					if (feof(cgi_pipe)) {
						fprintf(stderr,"[warn] Sadness: Pipe closed during headers.\n");
					}

					if (request_type == 3) {
						/*
						 * On a HEAD request, we're done here.
						 */
						fprintf(socket_stream, "\r\n");
						//pthread_detach(_waitthread);
						goto _next;
					}

					int enc_mode = 0;
					if (!strcmp(http_version, "HTTP/1.1")) {
						/*
						 * Set Transfer-Encoding to chunked so we can send
						 * pieces as soon as we get them and not have
						 * to read all of the output at once.
						 */
						cout<<"enc_mode=0"<<endl;
						fprintf(socket_stream, "Transfer-Encoding: chunked\r\n"); //设置传输编码
					} else {
						/*
						 * Not HTTP/1.1
						 * Use Connection: Close
						 */
						cout<<"enc_mode"<<endl;
						fprintf(socket_stream, "Connection: close\r\n\r\n");
						enc_mode = 1;
					}

					/*
					 * Sometimes, shit gets borked.
					 */
					if (strlen(buf) > 0) {
						fprintf(stderr, "[warn] Trying to dump remaining content.\n");
						fprintf(socket_stream, "\r\n%zX\r\n", strlen(buf));
						fwrite(buf, 1, strlen(buf), socket_stream);
					}

					/*
					 * 返回真正的CGI内容
					 */
					while (!feof(cgi_pipe)) {
						size_t read = -1;
						//从cgi_pipe读取到缓冲区,分块读取,每次读取CGI_BUFFER - 1,直到读不到,read表示实际读取到的容量
						read = fread(buf, 1, CGI_BUFFER - 1, cgi_pipe);
						if (read < 1) {
							/*
							 * Read nothing, we are done (or something broke)
							 */
							fprintf(stderr, "[warn] Read nothing on content without eof.\n");
							perror("[warn] Error on read");
							break;
						}
						if (enc_mode == 0) { //要遵循分块传输的标准 块大小\r\n内热\r\n
							/*
							 * Length of this chunk.
							 */
							fprintf(socket_stream, "\r\n%zX\r\n", read);
						}
						fwrite(buf, 1, read, socket_stream); //写到客户端
					}
					if (enc_mode == 0) { //分块传输结尾的格式
						/*
						 * We end `chunked` encoding with a 0-length block
						 */
						fprintf(socket_stream, "\r\n0\r\n\r\n");
					}

					/*
					 * Release memory for the waiting thread.
					 */
					//pthread_detach(_waitthread); //回收线程
					if (cgi_pipe) {
						/*
						 * If we need to, close this pipe as well.
						 */
						fclose(cgi_pipe); //关闭管道
					}

					/*
					 * Done executing CGI, move to next request or close
					 */
					if (enc_mode == 0) {//CGI结束
						/*
						 * HTTP/1.1
						 * Chunked encoding.
						 */
						goto _next;
					} else {
						/*
						 * HTTP/1.0
						 * Non-chunked, break the connection.
						 */
						delete_vector(queue);
						goto _disconnect;
					}
				}
#endif

//----------------------------------------------------处理正常的文件（非可执行文件）------------------------------------------
				/*
				 * Flat file: Status OK.
				 */
				fprintf(socket_stream, "HTTP/1.1 200 OK\r\n");
			}

			/*
			 * Server software header
			 */
			fprintf(socket_stream, "Server: " VERSION_STRING "\r\n");

			/*
			 * Determine the MIME type for the file.
			 */
			if (ext) {
				if (!strcmp(ext,".htm") || !strcmp(ext,".html")) {
					fprintf(socket_stream, "Content-Type: text/html\r\n");
				} else if (!strcmp(ext,".css")) {
					fprintf(socket_stream, "Content-Type: text/css\r\n");
				} else if (!strcmp(ext,".png")) {
					fprintf(socket_stream, "Content-Type: image/png\r\n");
				} else if (!strcmp(ext,".jpg")) {
					fprintf(socket_stream, "Content-Type: image/jpeg\r\n");
				} else if (!strcmp(ext,".gif")) {
					fprintf(socket_stream, "Content-Type: image/gif\r\n");
				} else if (!strcmp(ext,".pdf")) {
					fprintf(socket_stream, "Content-Type: application/pdf\r\n");
				} else if (!strcmp(ext,".manifest")) {
					fprintf(socket_stream, "Content-Type: text/cache-manifest\r\n");
				} else {
					fprintf(socket_stream, "Content-Type: text/unknown\r\n");
				}
			} else {
				fprintf(socket_stream, "Content-Type: text/unknown\r\n");
			}

			if (request_type == 3) { //HEAD请求
				/*
				 * On a HEAD request, stop here,
				 * we only needed the headers.
				 */
				fprintf(socket_stream, "\r\n");
				fclose(content);
				goto _next;
			}

			/*
			 * 计算打开请求文件的大小
			 */
			fseek(content, 0L, SEEK_END);//移动到末尾
			long size = ftell(content);//获得偏移的长度
			fseek(content, 0L, SEEK_SET);//移动到开头

			/*
			 * Send that length.
			 */
			fprintf(socket_stream, "Content-Length: %lu\r\n", size);
			fprintf(socket_stream, "\r\n");

			/*
			 * Read the file.
			 */
			char buffer[FLAT_BUFFER];
			fflush(stdout);
			while (!feof(content)) {    //????
				/*
				 * Write out the file as a stream until
				 * we hit the end of it.
				 */
				size_t read = fread(buffer, 1, FLAT_BUFFER-1, content);
				fwrite(buffer, 1, read, socket_stream);
			}

			fprintf(socket_stream, "\r\n");

			/*
			 * Close the file.
			 */
			fclose(content);
		}

_next:
		/*
		 * Clean up.
		 */
		fflush(socket_stream);
		free(_filename);
		delete_vector(queue);
	}

_disconnect:
	/*
	 * Disconnect.
	 */
	if (socket_stream) {
		fclose(socket_stream);
	}
	//断开socket的输入流和输出流
	shutdown(request->fd, 2);

	/*
	 * Clean up the thread
	 */
	if (request->thread) {
		pthread_detach(request->thread);
	}
	free(request);

	/*
	 * pthread_exit is implicit when we return...
	 */
	return NULL;
}

void start_httpd(unsigned short port, string doc_root, int nums)
{

	// if(nums==0){
	// 	thp = threadpool_create(10,100,100);/*创建线程池，池里最小10个线程，最大100，队列最大100*/
	// }else{
	// 	thp = threadpool_create(nums,100,100);/*创建线程池，池里最小10个线程，最大100，队列最大100*/
	// }

	cerr << "Starting server (port: " << port <<
		", doc_root: " << doc_root << ")" << endl;
	//终端输入进来的文件路径htdoc
	docs=doc_root.c_str();
	/*
	 * Initialize the TCP socket
	 */
	struct sockaddr_in sin;
	int serversock  = socket(AF_INET, SOCK_STREAM, 0);  //用于accept的socket
	sin.sin_family = AF_INET;
	sin.sin_port= htons(port);
	sin.sin_addr.s_addr = INADDR_ANY;

	/*
	 * Set reuse for the socket.
	 */
	int _true = 1;
	if (setsockopt(serversock, SOL_SOCKET, SO_REUSEADDR, &_true, sizeof(int)) < 0) {
		close(serversock);
		return;
	}

	/*
	 * Bind the socket.
	 */
	if (bind(serversock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		fprintf(stderr,"Failed to bind socket to port %d!\n", port);
		return;
	}

	/*
	 * Start listening for requests from browsers.
	 */
	listen(serversock, 50);
	printf("[info] Listening on port %d.\n", port);
	printf("[info] Serving out of '%s '.\n",docs.c_str());
	printf("[info] Server version string is " VERSION_STRING ".\n");

	/*
	 * Extensions
	 */
#if ENABLE_CGI
	printf("[extn] CGI support is enabled.\n");
#endif
#if ENABLE_DEFAULTS
	printf("[extn] Default indexes are enabled.\n");
#endif

	/*
	 * Use our shutdown handler.
	 */
	signal(SIGINT, handleShutdown);

	/*
	 * Ignore SIGPIPEs so we can do unsafe writes
	 */
	signal(SIGPIPE, SIG_IGN);

	/*
	 * Start accepting connections
	 */
	
	while (1) {
		/*
		 * Accept an incoming connection and pass it on to a new thread.
		 */
		cout<<"这是主进程"<<getpid()<<endl;
		unsigned int c_len;
		struct socket_request * incoming = (socket_request *)calloc(sizeof(struct socket_request),1);
		c_len = sizeof(incoming->address);
		_last_unaccepted = (void *)incoming;
		// 三次握手的代码层面感受
		incoming->fd = accept(serversock, (struct sockaddr *) &(incoming->address), &c_len); //serversock相当接待员
		_last_unaccepted = NULL;
		pthread_create(&(incoming->thread), NULL, handleRequest, (void *)(incoming));
		//threadpool_add_task(thp, handleRequest, (void *)incoming);
	}

	/*
	 * We will clean up when we receive a SIGINT.
	 */
 
}