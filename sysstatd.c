/* Standard 'includes' */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <pthread.h>
#include <netdb.h>
#include <error.h>


/* Our own defined files */
#include "sysstatd.h"
#include "threadpool.h"

/* Global variables */
static char *our_path;
static struct thread_pool *pool;

/* Declaration */
void doit(int fd);


/*  Just a helper function.  Prints an error message in the case of incorrect
 *  input, then exits normally.  
 */
static void usage(char *progname)
{
    printf("Usage: %s -h\n"
	   " -h print this help\n"
	   " -p port to accept HTTP clients requests\n"
	   " -r relayhost:port connect to relayhost on specified port\n"
	   " -R path specifies root directory of server reachable under '/files' prefix\n",
	   progname);
    
    exit(EXIT_SUCCESS);
}


/*  Main program driver.  Very long, a little messy.  
 *  Basically drives the whole web server.  Too much there to really descrie it 
 *  all in one header comment.
 */
int main(int argc, char **argv)
{
    /*  Don't use any pipes.  */
    signal(SIGPIPE, SIG_IGN);
   
    /* Error checking the input */
    if (argc < 2)
	usage(argv[0]);
    
    long port;
    bool relay = false;

    char c;
    /* The 'getopt' function profides a nice way to collect all the input 
     * arguments without being all messy with a buch of strncmp's
     */
    while ((c = getopt(argc, argv, "p:r:R:")) != -1)
    {

	    switch (c) 
        {
            case 'h':    //help
	            usage(argv[0]);
	            break;  //unnecessary because 'usage' exits, but just in case

    	    case 'p': 
            { //port number specified
    	        char *garbage = NULL;
    	        port = strtol(optarg, &garbage, 10);
                
    	        switch (errno) {
    	        case ERANGE://out of range (errno.h)
    		        printf("Please enter a valid port number");
    		        return -1;
    	        case EINVAL:
    		        printf("Please enter a valid port number");
    		        return -1;
    	        }
    
    	        break;
    	    }
    
    	    case 'r':
                relay = true;
                break;    //relay server option 
    
          	case 'R': //manually specify a directory for the server
    	        our_path = strdup(optarg);
    	    break;
    
    	    default:
                usage(argv[0]);
	    }
    }

    /* For if there is no path specified */
    if (our_path == NULL) our_path = "files";
    /* Initializations */
    list_init(&memory_list);
    int socketfd, optval = 1;
    intptr_t connfd;
    /* Creates a socket */
    if ((socketfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
	    fprintf(stderr, "Error opening socket.\n");
	    exit(1);
    }
    /* Set options on the socket */
    if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int)) < 0)
    {
    	fprintf(stderr, "Error in setting socket options. %s.\n", strerror(errno));
    	exit(1);
    }

    /* If relay server, send out connections */
    if(relay)
    {
        //int result = 0;
    	struct addrinfo *ai;
    	struct addrinfo hints;
    	memset(&hints, '\0', sizeof(hints));
    	hints.ai_flags = AI_ADDRCONFIG;
    	hints.ai_socktype = SOCK_STREAM;
    	char* name = strtok(argv[2], ":");
    	char* portname = strtok(NULL, "/");
    	//int e = 
    	getaddrinfo(name, portname, &hints, &ai);
    	//if (e != 0) error(EXIT_FAILURE, 0, "getaddrinfo: %s", strerror(e));
    	struct addrinfo *runp = ai;
    	while (runp != NULL)
    	{
    	   // int sock = socket(runp->ai_family, runp->ai_socktype, runp->ai_protocol);
    	    if(socketfd != -1)
    	    {
    	    	if(connect(socketfd, runp->ai_addr, runp->ai_addrlen) == 0)
    	    	{
    	    	    char *line = "IWantThisToBeDone\r\n";
    	    	    //printf("connected: %s\t %s\t %s\n", name, portname, line);
    	    	    write(socketfd, line, strlen(line));
    	    	    while(1)
    	    	    {
    	    		//printf("Inside loop \n");			
    	    	    	process_http(socketfd);
    	    		//printf("Skipped http\n");
    	    	    }
    	    	    //close(socketfd);
    	    	    //freeaddrinfo(ai);
    	    	    //return result;
    	    	}
    	    }
    	    runp = runp->ai_next;
    	}
	    return 1;
    }            

    struct sockaddr_in serveraddr;
    bzero((char *)&serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short) port);    

    /* Don't bind to a socket if this is a relay server */
    if(!relay)
    {
        /* Binds the socket.  Doesn't work with IPv6 and I can't figure out why */
        if (bind(socketfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0) 
        {
    	    fprintf(stderr, "Error binding socket. %s.\n", strerror(errno));
    	    exit(1);
        }
    }

    /* Set listener for the socket */
    if (listen(socketfd, 1024) < 0) 
    {
    	fprintf(stderr, "Error listening to socket.\n");
    	exit(1);
    }
    
    /* Create threadpool */
    pool = thread_pool_new(THREADS);
    /* Create a new, duplicate socket */
    struct sockaddr_in dupsock;

    /* Infinite loop that continuously listens for connections.  If found, 
     * submit it to the threadpool so it can be run in a different thread.
     */
    for (;;) 
    {
	    // new connected socket
	    int clientlen = sizeof(dupsock);

        /* Accept traffic, but put it into a new socket so we can still listen 
         * on the old one */
    	if ((connfd = accept(socketfd, (struct sockaddr *)&dupsock, (socklen_t *)&clientlen)) <= 0) 
        {
	         fprintf(stderr, "Error accepting connection.\n");
	         exit(1);
	    }

	    // execute request in a different thread
	    thread_pool_submit(pool, (thread_pool_callable_func_t)process_http, (int *)connfd);

    }
    /* When shutdown received */
    thread_pool_shutdown(pool);
    return 0;
}


/* Processes http requests.  Pretty straightforward. 
 * Support for loadavg, meminfo, and the widgets.
 * */

static void process_http(int fd)
{
    
    //printf("Processing http request\n");
    int is_static;
    struct stat sbuf;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char filename[MAXLINE], cgiargs[MAXLINE];
    rio_t rio;
    
    char pathbuf[64];
    sprintf(pathbuf, "./%s/", our_path); 
    
    Rio_readinitb(&rio, fd);
 
    /* I commented this out because I think there's a problem with clearing out
     * the buffers and stuff.  We'll just let the logic in main() take care of
     * the polling.
     */
 //   while (1) 
 //   {

    	/* Read request line and headers */
    	ssize_t x;
    	if ((x = Rio_readlineb(&rio, buf, MAXLINE)) <= 0);
        
        
    	sscanf(buf, "%s %s %s", method, uri, version);
    
        /* GET request */
    	if (strcasecmp(method, "GET"))
        {
    	    clienterror(fd, method, "501", "Not Implemented", "Server does not implement this method", version);
    	    /*continue;*/
    	}//endif
    
        /* Reading the request */
    	ssize_t size = read_requesthdrs(&rio);
    	if (size <= 0) 
        {
    	    if (strncmp(version, "HTTP/1.0", 8) == 0) {/* break;*/ }
    	    else { /*continue;*/ }
    	}//endif
    
    	/* Parse URI from GET request */
    	is_static = parse_uri(uri, filename, cgiargs);
    
    	/* If the request was loadavg:  /proc/loadavg */
    	if (strncmp(uri, "/loadavg\0", sizeof("/loadavg\0")-1) == 0 || strncmp(uri, "/loadavg?", sizeof("/loadavg?")-1) == 0) 
        {
    
    	    FILE *fp;
    	    fp = fopen("/proc/loadavg", "r");
    
    	    if (fp) 
            {
    
        		char loadbuf[256];
        		fgets(loadbuf, sizeof(loadbuf), fp);
        		fclose(fp);
        
        		float loadavg1;
        		float loadavg2;
        		float loadavg3;
        		int running;
        		char sep;
        		int total;
        
        		sscanf(loadbuf, "%f %f %f %d %c %d", &loadavg1, &loadavg2, &loadavg3, &running, &sep, &total);
        
                /* Make sure it's in JSON format */
        		char load_json[256];
        		sprintf(load_json, "{\"total_threads\": \"%d\", \"loadavg\": [\"%.2f\", \"%.2f\", \"%.2f\"], \"running_threads\": \"%d\"}", total, loadavg1, loadavg2, loadavg3, running);
        
                /* Support for callbacks */
        		char callbackbuf[256];	   
        		if (parse_callback(uri, callbackbuf) > 0) 
                {
        		    char returnbuf[256];
        		    sprintf(returnbuf, "%s(%s)", callbackbuf, load_json);
        		    response_ok(fd, returnbuf, "application/javascript", version);
        		}//endif
        		else 
                {
        		    response_ok(fd, load_json, "application/json", version);
        		}//endelse
	        }//endif
	    }//endif

	/* If the request was about meminfo: /proc/meminfo */
    	else if (strncmp(uri, "/meminfo\0", sizeof("/meminfo\0")-1) == 0 || strncmp(uri, "/meminfo?", sizeof("/meminfo?")-1) == 0) 
        {
    
    	    FILE *fp;
    	    fp = fopen("/proc/meminfo", "r");
    
    	    if (fp)
            {
    	    	char line[256];
    	    	char membuf[1024];
    	    	strcpy(membuf, "{");
    	    	int flag = 0;
                
                /* Read in */
    	    	while (fgets(line, sizeof(line), fp)) 
                {
    
    	    	    if (flag != 0) { strcat(membuf, ", "); }
    	    	    char title[64];
    	    	    long memory;
    	    	    char skip[5];
    
    	    	    sscanf(line, "%s %lu %s", title, &memory, skip);
    
    	    	    title[strlen(title)-1] = '\0';
    
    	    	    char tempbuf[256];
    	    	    sprintf(tempbuf, "\"%s\": \"%lu\"", title, memory);
    	    	    strcat(membuf, tempbuf);
    	    	    flag = 1;
    	    	}
    
    	    	fclose(fp);
    	    	strcat(membuf, "}");
    
    	    	char callbackbuf[256];	   
    	    	if (parse_callback(uri, callbackbuf) > 0) 
                {
    	    	    char returnbuf[256];
    	    	    sprintf(returnbuf, "%s(%s)", callbackbuf, membuf);
    	    	    response_ok(fd, returnbuf, "application/javascript", version);
    	    	}
    
    	    	else response_ok(fd, membuf, "application/json", version);
    	    }//endif
    	}//endif
    
        /* If looking for a specific file */
    	else if (strncmp(filename, pathbuf, strlen(pathbuf)) == 0)
        {
    
    	    // check that filename does not contain '..'
    	    if (strstr(filename, "..") != NULL)
            {
    		    clienterror(fd, filename, "403", "Forbidden", "Server couldn't read the file", version);
            }
    	    if (stat(filename, &sbuf) < 0)
            {
                clienterror(fd, filename, "404", "Not found", "Page Not Found", version);
            }
    	    if (is_static) 
            {
    
    		    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode))
                {
                    clienterror(fd, filename, "403", "Forbidden", "Server couldn't read the file", version);
                }
    		    serve_static(fd, filename, sbuf.st_size);
    	    }
    	    else 
            {
    
    		    if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode))
                {
                    clienterror(fd, filename, "403", "Forbidden", "Server couldn't run the CGI program", version);
                }
    		    serve_dynamic(fd, filename, cgiargs);
    	    }
    	}
    
        /* Runloop widget */
    	else if (strncmp(uri, "/runloop", strlen("/runloop")) == 0)
        {
    	    response_ok(fd, "<html>\n<body>\n<p>Started 15 second spin.</p>\n</body>\n</html>", "text/html", version);
    	    thread_pool_submit(pool, (thread_pool_callable_func_t)run_loop, (int *)0);
    	}
    
        /* Allocanon widget */
    	else if (strncmp(uri, "/allocanon", strlen("/allocanon")) == 0)
        {
    
    	    void *block = mmap(0, 67108864, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    
    	    if (block == MAP_FAILED)
    		perror("mmap() failed\n");
    
    	    else {
    
    		memset(block, '\0', 67108864);
    
    		struct memory *e = (struct memory *)malloc(sizeof(struct memory));
    		e->block = block;
    		list_push_back(&memory_list, &e->elem);
    
    		char buf[256];
    		sprintf(buf, "<html>\n<body>\n<p>64MB allocated in the heap.</p>\n</body>\n</html>");
    		response_ok(fd, buf, "text/html", version);
    	    }
    	}
    
        /* Freenon widget */
    	else if (strncmp(uri, "/freeanon", strlen("/freeanon")) == 0) 
        {
    
    	    if (list_size(&memory_list) > 0) 
            {
    
        		struct list_elem *e = list_pop_back(&memory_list);
        		struct memory *mem = list_entry(e, struct memory, elem);
        		int success = munmap(mem->block, 67108864);
        
        		if (success != 0)
        		    fprintf(stderr, "munmap() failed %s\n", strerror(errno));
        
        		else 
                {
        		    char buf[256];
        		    sprintf(buf, "<html>\n<body>\n<p>64MB freed successfully.</p>\n</body>\n</html>");
        		    response_ok(fd, buf, "text/html", version);
        		}
    	    }
    
    	    else 
            {
    		    char buf[256];
    		    sprintf(buf, "<html>\n<body>\n<p>You must allocate memory first using /allocanon</p>\n</body>\n</html>");
    		    response_ok(fd, buf, "text/html", version);
    	    }
    	}//endif
        /* Couldn't find the request */
    	else clienterror(fd, filename, "404", "Not Found", "Not found", version);
    
    	if (strncmp(version, "HTTP/1.0", 8) == 0) {/* continue;*/ }
   // }//endwhile
    //printf("finished http\n");
    //close(fd);
}

/*
 * read_requesthdrs - read and parse HTTP request headers
 * Shamelessley taken from Tiny server
 */
ssize_t read_requesthdrs(rio_t *rp) 
{
    char buf[MAXLINE];

    ssize_t size = Rio_readlineb(rp, buf, MAXLINE);
    while(strcmp(buf, "\r\n")) 
    {
	    size = Rio_readlineb(rp, buf, MAXLINE);
	    if (size <= 0) break;
    }
    return size;
}

/*
 * parse_uri - parse URI into filename and CGI args
 *             return 0 if dynamic content, 1 if static
 * Taken from Tiny server and slightly modified
 */
int parse_uri(char *uri, char *filename, char *cgiargs) 
{
    char *ptr;

    if (!strstr(uri, "cgi-bin")) 
    {
	    strcpy(cgiargs, "");
	    strcpy(filename, ".");
	    strcat(filename, uri);
	    if (uri[strlen(uri)-1] == '/')
        {
	        char pathbuf[64];
	        sprintf(pathbuf, "%s/home.html", our_path);
	        strcat(filename, pathbuf);
	    }
	    return 1;
    }

    else /* Dynamic content */
    {                                                    //line:netp:parseuri:isdynamic
	    ptr = index(uri, '?');                           //line:netp:parseuri:beginextract
	    if (ptr) 
        {
	        strcpy(cgiargs, ptr+1);
	        *ptr = '\0';
    	}
	    else strcpy(cgiargs, "");                         //line:netp:parseuri:endextract
    	strcpy(filename, ".");                           //line:netp:parseuri:beginconvert2
    	strcat(filename, uri);                           //line:netp:parseuri:endconvert2
	    return 0;
    }
}

/*
 * Build and send an HTTP response.  Just a helper function.  
 */
void response_ok(int fd, char *msg, char *content_type, char *version)
{
    char buf[MAXLINE];
    char body[MAXBUF];

    sprintf(body, "%s", msg);
    sprintf(buf, "%s 200 OK\r\n", version);
    if (strncmp(version, "HTTP/1.0", strlen("HTTP/1.0")) == 0) sprintf(buf, "Connection: close\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: %s\r\n", content_type);
    
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));   
    Rio_writen(fd, body, strlen(body));
}

/*
 * clienterror - returns an error message to the client
 * Taken from Tiny server
 */
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg, char *version) 
{
    char buf[MAXLINE], body[MAXBUF];

    /* Build the HTTP response body */
    sprintf(body, "<html><title>Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>Sysstatd Web server</em>\r\n", body);

    /* Print the HTTP response */
    sprintf(buf, "%s %s %s\r\n", version, errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}

/*
 * parse_callback
 * parse URI and returns callback buffer size
 * return 0 if no callback exists
 */
static size_t parse_callback(char *uri, char *callback)
{
    char *ptr;
    ptr = index(uri, '?');
    fflush(stdout);
    
    if (ptr) 
    {

    	while (ptr) 
        {
    	    ptr++;
    	    if (strncmp(ptr, "callback=", sizeof("callback=")-1) == 0) 
            {
    
        		ptr = index(ptr, '=');
        		int pass = 0;
        
        		if (ptr) 
                {
        		    ptr++;
           		    int i = 0;
           		    while (*ptr && *ptr != '&') 
                    {
               			int c = ptr[0];
        	    		if ((c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c == 46 || (c >= 48 && c <= 57) || c == 95)
                        {
        			        callback[i] = *ptr;
        			        i++;
        			        ptr++;
        			    }
        			    else pass = 1;
        		    }
        		    if (pass == 0) 
                    {
        			    callback[i] = '\0';
        			    return sizeof(callback);
        		    }
        		}
        
    	    }
    	    else ptr = index(ptr, '&');
    	}
    }
    
    return 0;
}

/*
 * 15 second runloop widget
 */
static void run_loop(void) {
    time_t start;
    start = time(NULL);
    while ((time(NULL) - start) < 15) 
    {
	    continue;
    }
    return;
}

/*
 * serve_static - copy a file back to the client
 * Taken from Tiny server
 */
void serve_static(int fd, char *filename, int filesize) 
{
    int srcfd;
    char *srcp, filetype[MAXLINE], buf[MAXBUF];
 
    /* Send response headers to client */
    get_filetype(filename, filetype);       //line:netp:servestatic:getfiletype
    sprintf(buf, "HTTP/1.0 200 OK\r\n");    //line:netp:servestatic:beginserve
    sprintf(buf, "%sServer: Sysstatd Web Server\r\n", buf);
    sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
    sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);
    Rio_writen(fd, buf, strlen(buf));       //line:netp:servestatic:endserve

    /* Send response body to client */
    srcfd = open(filename, O_RDONLY, 0);    //line:netp:servestatic:open
    srcp = mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);//line:netp:servestatic:mmap
    Rio_writen(fd, srcp, filesize);         //line:netp:servestatic:write
    munmap(srcp, filesize);                 //line:netp:servestatic:munmap
    close(srcfd);                           //line:netp:servestatic:close
}

/*
 * get_filetype - derive file type from file name
 * Taken from Tiny server
 */
void get_filetype(char *filename, char *filetype) 
{
    if (strstr(filename, ".html")) strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif")) strcpy(filetype, "image/gif");
    else if (strstr(filename, ".jpg")) strcpy(filetype, "image/jpeg");
    else if (strstr(filename, ".js")) strcpy(filetype, "text/javascript");
    else if (strstr(filename, ".css")) strcpy(filetype, "text/css");
    else strcpy(filetype, "text/plain");
}  

/*
 * serve_dynamic - run a CGI program on behalf of the client
 * Taken from Tiny server and slightly modified
 */
void serve_dynamic(int fd, char *filename, char *cgiargs) 
{
    char buf[MAXLINE], *emptylist[] = { NULL };

    /* Return first part of HTTP response */
    sprintf(buf, "HTTP/1.0 200 OK\r\n"); 
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Sysstatd Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));
  
    if (fork() == 0)
    { /* child */ //line:netp:servedynamic:fork
	    /* Real server would set all CGI vars here */
	    setenv("QUERY_STRING", cgiargs, 1); //line:netp:servedynamic:setenv
	    dup2(fd, STDOUT_FILENO);         /* Redirect stdout to client */ //line:netp:servedynamic:dup2
	    execve(filename, emptylist, environ); /* Run CGI program */ //line:netp:servedynamic:execve
    }

    if (wait(NULL) < 0)     
    {
	    fprintf(stderr, "Wait Error.\n");
    	exit(1);
    }
}



/*
 * doit - handle one HTTP request/response transaction
 * We're not going to use this one, but it's here for reference.
 */
/* $begin doit */
void doit(int fd) 
{
    int is_static;
    struct stat sbuf;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char filename[MAXLINE], cgiargs[MAXLINE];
    rio_t rio;
  
    /* Read request line and headers */
    Rio_readinitb(&rio, fd);
    Rio_readlineb(&rio, buf, MAXLINE);                   //line:netp:doit:readrequest
    sscanf(buf, "%s %s %s", method, uri, version);       //line:netp:doit:parserequest
    if (strcasecmp(method, "GET")) {                     //line:netp:doit:beginrequesterr
       clienterror(fd, method, "501", "Not Implemented",
                "Tiny does not implement this method", "1.1");
        return;
    }                                                    //line:netp:doit:endrequesterr
    read_requesthdrs(&rio);                              //line:netp:doit:readrequesthdrs

    /* Parse URI from GET request */
    is_static = parse_uri(uri, filename, cgiargs);       //line:netp:doit:staticcheck
    if (stat(filename, &sbuf) < 0) {                     //line:netp:doit:beginnotfound
	clienterror(fd, filename, "404", "Not found",
		    "Tiny couldn't find this file", "1.1");
	return;
    }                                                    //line:netp:doit:endnotfound

    if (is_static) { /* Serve static content */          
	if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) { //line:netp:doit:readable
	    clienterror(fd, filename, "403", "Forbidden",
			"Tiny couldn't read the file", "1.1");
	    return;
	}
	serve_static(fd, filename, sbuf.st_size);        //line:netp:doit:servestatic
    }
    else { /* Serve dynamic content */
	if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) { //line:netp:doit:executable
	    clienterror(fd, filename, "403", "Forbidden",
			"Tiny couldn't run the CGI program", "1.1");
	    return;
	}
	serve_dynamic(fd, filename, cgiargs);            //line:netp:doit:servedynamic
    }
}
/* $end doit */

