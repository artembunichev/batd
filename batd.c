/*
	batd -- a battery daemon for X11.

	It reports when battery is low (see config.h) by the
	means of popping an X(7) powered window up on the screen.
*/

#include<sys/ioctl.h> /* ioctl(2) */
#include<sys/wait.h> /* wait(2) */

#include<dev/acpica/acpiio.h> /* acpi_battery(4) types */

#include<fcntl.h> /* open(2) */
#include<stdio.h> /* dprintf(3) */
#include<string.h> /* strlen(3) */
#include<unistd.h> /* fork(2) and sleep(3) */

#include<X11/Xlib.h> /* base Xlib functions (X(7)) */
#include<X11/Xutil.h> /* XClassHint(3)  */

#include"config.h" /* custom configuration */


/*
	A structures that holds the information about physical
	size of a string printed on the screen with the font.
	It's used by `strsz' function.
*/
typedef struct {
	/* width. */
	int w;
	/* height. */
	int h;
	/*
		next are equivalents for the following
		`XCharStruct' members:
	*/
	/* ascent. */
	int asc;
	/* descent. */
	int desc;
	/* rbearing. */
	int rb;
	/* lbearing. */
	int lb;
} sz;


/* Display and main window. */
Display* dpl;
Window w;
/* window width and height (computed). */
int ww, wh;

/* button. */
Window btn;
/* button width and height. */
int bw, bh;


/* Graphics Context for main window. */
GC gc;

/* Font. */
XFontStruct* font;
/* physical sizes on the screen for main and "OK" messages. */
sz msgsz;
sz oksz;

/* "WM_DELETE_WINDOW" X atom.*/
Atom wmdel;


/* initialize the font. */
void dofont() {
	font = XLoadQueryFont(dpl, fontname);
}

/*
	Calculate a physical size of a string when it's printed
	on the screen with the current font.
	The second argument is a pointer to resulting structure.
*/
void strsz(char* msg, sz* ret) {
	/* dummy variable for values we don't need. */
	int dum;
	XCharStruct xch;
	/* Make a request for string parameters. */
	XTextExtents(font, msg, strlen(msg), &dum,
		&dum, &dum, &xch);
	/* And put them into the resulting structure. */
	ret->w = xch.rbearing - xch.lbearing;
	ret->h = xch.ascent + xch.descent;
	ret->asc = xch.ascent;
	ret->desc = xch.descent;
	ret->rb = xch.rbearing;
	ret->lb = xch.lbearing;
}

/* initialize GC. */
void dogc() {
	XGCValues* gcv = 0;
	gc = XCreateGC(dpl, w, 0, gcv);
	XSetFont(dpl, gc, font->fid);
}

/* window routine: create, size and render. */
void dowin(char* msg) {
	/*
		Before creating the window, we need to determine the
		size of it. It depends on the (physical) size of a
		message and size of an "OK" button.
	*/
	/* request calculation of the message physical size. */
	strsz(msg, &msgsz);
	ww = 2 * WPADX + msgsz.w;
	/* calculate the physical size on the screen for "OK". */
	strsz("OK", &oksz);
	bw = 2 * BPADX + oksz.w;
	bh = 2 * BPADY + oksz.h;
	/*
		now, when we now the size of a button, we can
		calculate the size of a whole window.
	*/
	wh = 2 * WPADY + msgsz.h + WGAP + bh;
	/*
		Main window will be positioned at a screen center.
	*/
	Screen* scr = DefaultScreenOfDisplay(dpl);
	/* calculate the actual position for window. */
	int wx = WidthOfScreen(scr) / 2 - ww / 2;
	int wy = HeightOfScreen(scr) / 2 - wh / 2;
	/* create the main window. */
	w = XCreateSimpleWindow(dpl, DefaultRootWindow(dpl), wx, wy,
		ww, wh, 0, 0, 0xFFFFFF);
	/* calculate a position within the main window for button. */
	int bx = (ww - bw) / 2;
	int by = wh - WPADY - bh;
	/*
		Create the button window.
		In X11 there are no buttons, we deal only with windows.
		So, in X's terms, button is a window too - a window, that
		is a child of our main window.
	*/
	btn = XCreateSimpleWindow(dpl, w, bx, by, bw, bh,
		0, 0, 0x000000);
	/*
		Set main window name hints (instance and class),
		and window name.
		Last is basically used by window managers to print
		the name of the window in the titlebar.
		First two can be used as somewhat of simple identifiers
		for window (e.g. in i3(1) config).
	*/
	XClassHint* clshnt = XAllocClassHint();
	clshnt -> res_name = "batd";
	clshnt -> res_class = "batd";
	XSetClassHint(dpl, w, clshnt);
	XFree(clshnt);
	XStoreName(dpl, w, "batd");
	/*
		subscribe to `WM_DELETE_WINDOW' event;
		the window manager reports it when the window is killed.
	*/
	wmdel = XInternAtom(dpl, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpl, w, &wmdel, 1);
	/*
		Now tell the X server which events we want to receive.
		For main window we only need the `Expose' event, it'll
		tell us when to redraw message.
		But for button window we also want to know when the
		click is performed, and for that we need `Press' and
		`Release' button events.
	*/
	XSelectInput(dpl, w, ExposureMask);
	XSelectInput(dpl, btn, ButtonPressMask | ButtonReleaseMask
		| ExposureMask);
	/* And finally display our windows. */
	XMapWindow(dpl, w);
	XMapWindow(dpl, btn);
	XSync(dpl, False);
}

/*
	draw a string on the window.
	`sx' and `sy' are coordinates of a top-left point
	we want our message to be actually printed at. however,
	we can not just pass them into the `XDrawString' as is - we
	also need to bear in mind that the font has such thing as
	"ascent" (and also "descent", but in this case it seems that
	we don't need it). simply speaking, we can think about "ascent"
	as of the most top point of a font character. Example: letter
	"T" is higher than letter "a" (in most fixed bitmap fonts),
	hence, every character in this font, that is smaller than "T",
	must have some space in top of it so that all the characters
	be the same size. And now imagine, that we want to draw a string
	"ae" on the screen. Both "a" and "e" are not that high as "T" and
	they have this padding on top of them (empty space). Hence,
	`XDrawString' will draw them at the coordinates we specify
	with these paddings on top. And this is not what we want.
	What we do to fix that - we calculate the maximum height
	among characters in our string (call it an "ascent") and
	we shift down the original "y" coordinate by this value.
*/
void drwmsg(Window w, int sx, int sy, char* msg,
 sz* msz, unsigned long fg) {
	XSetForeground(dpl, gc, fg);
	XClearWindow (dpl, w);
	XDrawString(dpl, w, gc, sx,
		sy + msz->asc, msg, strlen(msg));
}

/*
	print all the messages on the window.
	this function should be called on every `Expose' event.
*/
void drwmsgs(char* msg) {
	/* print the message on the main window. */
	drwmsg(w, WPADX, WPADY, msg, &msgsz, 0x000000);
	/* print the "OK" on the button. */
	drwmsg(btn, BPADX, BPADY, "OK", &oksz, 0xFFFFFF);
}

/*
	Create an X-powered window, write a message text into it
	and pop it up to the screen.
	The window consists of a message itself and a button that
	says "OK", the click on which will close the window.
	This function is supposed to be called via fork(2) from the
	`main' function. `main', in its turn, waits till his child
	dies and do not dare to check the battery till that.
	Therefore, this function should return its execution status code.
*/
int popwin(char* msg) {
	/* Open the display. */
	dpl = XOpenDisplay(0);
	if (!dpl) {
		dprintf(2, "[batd]: can't open display.");
		return 1;
	}
	dofont();
	dowin(msg);
	dogc();
	/* is our button pressed. */
	char presd = 0;	
	/* is our main window loop stopped. */
	char quit = 0;
	XEvent xev;
	/* enter main window loop. */
	while (!quit) {
		XNextEvent(dpl,&xev);		
		switch(xev.type) {
		/* handle closing by window manager. */
		case ClientMessage: {
			if (xev.xclient.data.l[0] == wmdel) {
				quit = 1;
			}
			break;
		}
		/* redraw everything. */
		case Expose: {
			drwmsgs(msg);
			break;
		}
		case ButtonPress: {
			XButtonEvent xpev = xev.xbutton;
			/*
				we don't even check for the window this
				event took place in, since we've the only
				window we've subscribed to this event is
				our button window.
			*/
			presd = 1;			
			break;
		}
		case ButtonRelease: {
			XButtonEvent xrev = xev.xbutton;
			/*
				we want to register only LMB clicks (Button1 in Xlib).
				as far as we know that there is only our
				button receive this event, then the response
				will contain `x' and `y' that are relative for
				our button. hence, if the button release
				happened *outside* the actual button, the `x'
				or the `y' will be negative. and that's how
				we tell click doesn't qualify.
			*/
			if (xrev.state == Button1Mask && xrev.x >= 0
			 && xrev.y >= 0 && presd) {
				quit = 1;
			}
			presd = 0;
			break;
		}
		}
	}
	/* close the display and destroy all the used resources. */
	XCloseDisplay(dpl);
	return 1;
}

int main() {
	/* file descriptor for ACPI device. */
	int acpifd;
	/* data structures we will use to obtain information from it. */
	union acpi_battery_ioctl_arg batio;
	struct acpi_battinfo* batinf;
	acpifd = open("/dev/acpi", O_RDONLY);
	if (acpifd == -1) {
		dprintf(2, "[batd]: can't open /dev/acpi file.\n");
		return 1;
	}
	/*
		here we will but a messages that says that we're low
		on battery and then pass it to our pop-up window.
	*/
	char* msg;
	/* the main program loop. */
	while(1) {
		msg = 0;
		/*
			Here is *very* important to initialize the `unit' member first.
			According to ioctl(2) page, it uses a third argument for
			passing some data to a device. the actual type of data
			depends on a particular request we're making. Speaking of
			a request, acpi_battery(4) manual says that as input argument
			it takes a battery unit number. So we need to specify it.
			`ACPI_BATTERY_ALL_UNITS' will obtain a summary about all
			the battery units attached.
			See more about this request in /usr/src/sys/dev/acpica/acpi_battery.c.
		*/
		batio.unit = ACPI_BATTERY_ALL_UNITS;
		if (ioctl(acpifd, ACPIIO_BATT_GET_BATTINFO, &batio) == -1) {
			dprintf(2, "[batd]: can't send an ioctl(2) request to /dev/acpi.\n");
			return 1;
		}
		batinf = &(batio.battinfo);
		/*
			Complain about low battery only if we're *not*
			already charging.
			Note: according to comment notes for battery status macros
			in /usr/src/sys/dev/acpica/acpiio.h, status `0' does not
			appear in the ACPI specification and is synthetic, so it
			does not have a macro. As I figured, it has somewhat-similar
			meaning with `charging' status, so here we count it too.
		*/
		if (batinf->cap <= BATLOW &&
 		 batinf->state != ACPI_BATT_STAT_CHARGING) {
			msg = LOWMSG;
		}
		/*
			so, we are not low on battery,
			so sleep for a while and then check again.
		*/
		if (!msg) {
			sleep(INTERVAL);
			continue;
		}
		/*
			But in case we are low, we want
			to create a child process that will display
			this message (make it visible).
		*/
		pid_t pid;
		pid = fork();
		if (pid == -1) {
			dprintf(2, "[batd]: can't fork(2).\n");
			return 1;
		}
		/* We're child. */
		if (!pid) {
			/*
				`popwin' returns its status code, so here
				we want to return the result of this function.
			*/
			return popwin(msg);
		}
		/* We're parent. */
		else {
			/*
				Well, we just opened the child window, so we want
				to wait till it dies, then sleep a bit and after
				that check the battery again.
			*/
			/* dummy value, we won't use it. */
			int sts;
			wait(&sts);
			sleep(INTERVAL);
		}
	}
	/* Yep, that's it. */
	return 0;
}
