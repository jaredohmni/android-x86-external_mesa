
/*
 * Sample server that just keeps first available window mapped.
 * 
 * It also reads and echos anything that happens on stdin as an
 * example of tracking events from sources other than miniglx clients.
 * 
 * It reads & writes without blocking, so that eg. piping a lot of
 * text to stdin and then hitting 'ctrl-S' on the output stream won't
 * cause it to stop handling miniglx events.
 *
 * See select_tut in the linux manual pages for a good overview of the 
 * select(2) system call.
 */


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <GL/gl.h>
#include <GL/miniglx.h>
#include <errno.h>
#include <assert.h>

struct client {
   struct client *next;
   Window windowid;
   int mappable;
};

struct client *clients = 0, *mapped_client = 0;

#define BUFSZ 4096
char rbuf[BUFSZ];
int rbuf_count;


static struct client *find_client( Window id )
{
   struct client *c;

   for (c = clients ; c ; c = c->next)
      if (c->windowid == id)
	 return c;

   return 0;
}

int main( int argc, char *argv[] )
{
   Display *dpy;
   XEvent ev;
   int autostart = 0;

   if (argc == 2 && strcmp(argv[1], "-autostart") == 0)
      autostart = 1;

   dpy = __miniglx_StartServer(NULL);
   if (!dpy) {
      fprintf(stderr, "Error: __miniglx_StartServer failed\n");
      return 1;
   }

   /* How is vt switching communicated through the XNextEvent interface?
    */
   while (1) {
      int r, n;
      struct timeval tv;
      fd_set rfds, wfds;
      int bored = 0;

      FD_ZERO(&rfds);
      FD_ZERO(&wfds);
      tv.tv_sec = 1;
      tv.tv_usec = 0;

      if (rbuf_count) {
	 FD_SET( 1, &wfds );	/* notify when we can write out buffer */
	 n = 1;
      }
      else {
	 FD_SET( 0, &rfds );	/* else notify when new data to read */
	 n = 0;
      }

      /* __miniglx_Select waits until any of these file groups becomes
       * readable/writable/etc (like regular select), until timeout
       * expires (like regular select), until a signal is received
       * (like regular select) or until an event is available for
       * XCheckMaskEvent().
       */
      r = __miniglx_Select( dpy, n+1, &rfds, &wfds, 0, &tv );

      /* This can happen if select() is interrupted by a signal:
       */
      if (r < 0 && errno != EINTR && errno != EAGAIN) {
	 perror ("select()");
	 exit (1);
      }

      if (tv.tv_sec == 0 && tv.tv_usec == 0)
	 bored = 1;

      /* Check and handle events on our local file descriptors
       */
      if (FD_ISSET( 0, &rfds )) {
	 /* Something on stdin */	 
	 assert(rbuf_count == 0);
	 r = read(0, rbuf, BUFSZ);	 
	 if (r < 1) {
	    perror("read");	
	    abort();
	 }
	 rbuf_count = r;
      }

      if (FD_ISSET( 1, &wfds )) {
	 /* Can write to stdout */
	 assert(rbuf_count > 0);
	 r = write(1, rbuf, rbuf_count);	 
	 if (r < 1) {
	    perror("write");
	    abort();
	 }
	 rbuf_count -= r;
	 if (rbuf_count) 
	    memmove(rbuf + r, rbuf, rbuf_count);
      }


      /* Check and handle events generated by miniglx:
       */
      while (XCheckMaskEvent( dpy, ~0, &ev )) {
	 struct client *c;
	 bored = 0;

	 fprintf(stderr, "Received event %d\n", ev.type);

	 switch (ev.type) {
	 case CreateNotify: 
	    fprintf(stderr, "CreateNotify -- new client\n");
	    c = malloc(sizeof(*c));
	    c->next = clients;
	    c->windowid = ev.xcreatewindow.window;
	    c->mappable = False;
	    clients = c;
	    break;

	 case DestroyNotify:
	    fprintf(stderr, "DestroyNotify\n");
	    c = find_client(ev.xdestroywindow.window);
	    if (!c) break;
	    if (c == clients)
	       clients = c->next;
	    else {
	       struct client *t;
	       for (t = clients ; t->next != c ; t = t->next)
		  ;
	       t->next = c->next;
	    }

	    if (c == mapped_client) 
	       mapped_client = 0;

	    free(c);
	    break;

	 case MapRequest:
	    fprintf(stderr, "MapRequest\n");
	    c = find_client(ev.xmaprequest.window);
	    if (!c) break;
	    c->mappable = True;
	    break;

	 case UnmapNotify:
	    fprintf(stderr, "UnmapNotify\n");
	    c = find_client(ev.xunmap.window);
	    if (!c) break;
	    c->mappable = False;
	    if (c == mapped_client)
	       mapped_client = 0;
	    break;

	 default:
	    break;
	 }
      }


      /* Search for first mappable client if none already mapped.
       */
      if (!mapped_client) {
	 struct client *c;
	 for (c = clients ; c ; c = c->next) {
	    if (c->mappable) {
	       XMapWindow( dpy, c->windowid );
	       mapped_client = c;
	       break;
	    }
	 }
	 if (!clients && autostart) {
	    system("nohup ./texline &");
	    system("nohup ./manytex &");
	 }
      }
      else if (bored) {
	 struct client *c;
	 /* bored of mapped client now, let's try & find another one */
	 for (c = mapped_client->next ; c && !c->mappable ; c = c->next)
	    ;
	 if (!c)
	    for (c = clients ; c && !c->mappable ; c = c->next)
	       ;
	 if (c && c != mapped_client) {
	    XUnmapWindow( dpy, mapped_client->windowid );
	    XMapWindow( dpy, c->windowid );
	    mapped_client = c;
	 }
	 else 
	    fprintf(stderr, "I'm bored!\n");
      }
   }

   XCloseDisplay( dpy );

   return 0;
}
