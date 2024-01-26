#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <linux/fb.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xdamage.h>
// #include <X11/extensions/Xfixes.h> // too slow
#include "fbink.h"

#define ERRCODE(e) (-(e))

#define max(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

#define min(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _b : _a; })

// Globals:
int fbfd = -1;
FBInkConfig fbink_cfg = { 0 };

typedef struct myrect
{
    unsigned int x;
    unsigned int y;
    unsigned int width;
    unsigned int height;
} myrect_t;

int rect1InsideRect2(myrect_t rect1, myrect_t rect2)
{
    if( (rect1.x >= rect2.x && rect1.x <= rect2.x+rect2.width) || (rect1.x+rect1.width >= rect2.x && rect1.x+rect1.width <= rect2.x+rect2.width) ){
        if(rect1.y >= rect2.y && rect1.y <= rect2.y+rect2.height)
            return true;
        if(rect1.y+rect1.height >= rect2.y && rect1.y+rect1.height <= rect2.y+rect2.height)
            return true;
    }
}

int rectsIntersect(myrect_t rect1, myrect_t rect2)
{
    return rect1InsideRect2(rect1, rect2) || rect1InsideRect2(rect2,rect1);
}

myrect_t rectsMerge(myrect_t rect1, myrect_t rect2)
{
    /*printf("\nMerging:\n");
    printf("rect1: %i %i %i %i\n", rect1.x, rect1.y, rect1.width, rect1.height);
    printf("rect1: %i %i %i %i\n", rect2.x, rect2.y, rect2.width, rect2.height);*/
    myrect_t r;
    r.x = min(rect1.x, rect2.x);
    r.y = min(rect1.y, rect2.y);
    r.width = max(rect1.x+rect1.width, rect2.x+rect2.width)-r.x;
    r.height = max(rect1.y+rect1.height, rect2.y+rect2.height)-r.y;
    //printf("r    : %i %i %i %i\n", r.x, r.y, r.width, r.height);
    return r;
}

unsigned int msElapsedSince(struct timespec since)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    unsigned int sec_elapsed = now.tv_sec-since.tv_sec;
    int msec_elapsed = ((long)now.tv_nsec-(long)since.tv_nsec)/1e6;
    //printf("Elapsed: %is %dms\n", sec_elapsed, msec_elapsed);
    return sec_elapsed*1000+msec_elapsed;   
}

int main(int argc, char *argv[])
{
    Display* display = XOpenDisplay(NULL);
    Window root = DefaultRootWindow(display);

    //XDAMAGE INIT
    int damage_event, damage_error, test;
    test = XDamageQueryExtension(display, &damage_event, &damage_error);
    Damage damage = XDamageCreate(display, root, XDamageReportRawRectangles);
    
    XEvent event;
    XDamageNotifyEvent *devent;
     
    // Assume success, until shit happens ;)
	int rv = EXIT_SUCCESS;

	// Init FBInk
	int fbfd = -1;
	// Open framebuffer and keep it around, then setup globals.
	if ((fbfd = fbink_open()) == ERRCODE(EXIT_FAILURE))
    {
		fprintf(stderr, "Failed to open the framebuffer, aborting . . .\n");
		rv = ERRCODE(EXIT_FAILURE);
		goto cleanup;
	}
	if (fbink_init(fbfd, &fbink_cfg) == ERRCODE(EXIT_FAILURE))
    {
		fprintf(stderr, "Failed to initialize FBInk, aborting . . .\n");
		rv = ERRCODE(EXIT_FAILURE);
		goto cleanup;
	}
    fbink_cfg.dithering_mode = HWD_PASSTHROUGH; //HWD_ORDERED;
    fbink_cfg.is_verbose = true;

    struct timespec firstRefresh;
    bool shouldRefresh = false;

    myrect_t square;
    // initialize rectangle
    square.x = square.y = square.width = square.height = 0;	
    fbink_refresh(fbfd, 0,0,0,0, &fbink_cfg);
    clock_gettime(CLOCK_REALTIME, &firstRefresh);
	
    while (1)
    {  
        // if the first refresh event timestamp was more than 50ms ago AND there is a rectangle  
        if (msElapsedSince(firstRefresh) > 50 && square.width * square.height) {

            printf ("Refreshing region x=%d, y=%d, w=%d, h=%d\n",
                        square.x, square.y, 
                        square.width, square.height);            

            // do a refresh
            fbink_refresh(fbfd, square.y, square.x, square.width, square.height, &fbink_cfg);
                       
            // clean accumulated rectangle
            shouldRefresh = false;

            // reset rectangle
            square.x = square.y = square.width = square.height = 0;

            printf("msElapsedSince! tdiffms:%i\n", msElapsedSince(firstRefresh));
        }
	    
	// if there are pending updates OR there is not first refresh event timestamp
        if ( XPending(display) || !shouldRefresh ) {
            
            // get next event or lock until one arrives
            XNextEvent(display,&event);

	    // if there is not a first refresh event timestamp
            if(!shouldRefresh) {
                clock_gettime(CLOCK_REALTIME, &firstRefresh);
                shouldRefresh = true;
            }

            // accumulate rectangle
            printf("Got event! type:%i\n", event.type);
            devent = (XDamageNotifyEvent*)&event;
            printf ("Damage notification in window %d\n", devent->drawable);
            XDamageSubtract(display, devent->damage, None, None);

            printf ("Accumulating rectangle x=%d, y=%d, w=%d, h=%d\n",
                        devent->area.x, devent->area.y, 
                        devent->area.width, devent->area.height);
            
            myrect_t area;
            area.x = devent->area.x;
            area.y = devent->area.y;
            area.width = devent->area.width;
            area.height = devent->area.height;

            if ( square.x == 0 && square.y == 0 && square.width == 0 && square.height == 0 ) {
                printf ("New rectangle\n");
                square = area;
            } else {
                printf ("Accumulated rectangle %d %d %d %d\n", square.x, square.y, square.width, square.height);
                square = rectsMerge(square, area);   
            }   
        }	    
    }
    XCloseDisplay(display);
        
    
    // Cleanup
cleanup:
	if (fbink_close(fbfd) == ERRCODE(EXIT_FAILURE))
    {
		fprintf(stderr, "Failed to close the framebuffer, aborting . . .\n");
		rv = ERRCODE(EXIT_FAILURE);
	}

	return rv;

}
