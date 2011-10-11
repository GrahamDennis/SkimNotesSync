/*
File:       Watcher.c

Abstract:   A simple demonstration of the FSEvents API.

Version: <1.3>

Disclaimer: IMPORTANT:  This Apple software is supplied to you by 
Apple Inc. ("Apple") in consideration of your agreement to the
following terms, and your use, installation, modification or
redistribution of this Apple software constitutes acceptance of these
terms.  If you do not agree with these terms, please do not use,
install, modify or redistribute this Apple software.

In consideration of your agreement to abide by the following terms, and
subject to these terms, Apple grants you a personal, non-exclusive
license, under Apple's copyrights in this original Apple software (the
"Apple Software"), to use, reproduce, modify and redistribute the Apple
Software, with or without modifications, in source and/or binary forms;
provided that if you redistribute the Apple Software in its entirety and
without modifications, you must retain this notice and the following
text and disclaimers in all such redistributions of the Apple Software. 
Neither the name, trademarks, service marks or logos of Apple Inc. 
may be used to endorse or promote products derived from the Apple
Software without specific prior written permission from Apple.  Except
as expressly stated in this notice, no other rights or licenses, express
or implied, are granted by Apple herein, including but not limited to
any patent rights that may be infringed by your derivative works or by
other works in which the Apple Software may be incorporated.

The Apple Software is provided by Apple on an "AS IS" basis.  APPLE
MAKES NO WARRANTIES, EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION
THE IMPLIED WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY AND FITNESS
FOR A PARTICULAR PURPOSE, REGARDING THE APPLE SOFTWARE OR ITS USE AND
OPERATION ALONE OR IN COMBINATION WITH YOUR PRODUCTS.

IN NO EVENT SHALL APPLE BE LIABLE FOR ANY SPECIAL, INDIRECT, INCIDENTAL
OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) ARISING IN ANY WAY OUT OF THE USE, REPRODUCTION,
MODIFICATION AND/OR DISTRIBUTION OF THE APPLE SOFTWARE, HOWEVER CAUSED
AND WHETHER UNDER THEORY OF CONTRACT, TORT (INCLUDING NEGLIGENCE),
STRICT LIABILITY OR OTHERWISE, EVEN IF APPLE HAS BEEN ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

Copyright (C) 2007 Apple Inc. All Rights Reserved.

*/

//
// This program is a simple example of using the FSEvents
// framework that monitors a directory hierarchy and keeps
// track of the total size of data contained in it.  When
// a directory inside of it changes, it recalculates the
// size of that directory and updates the total size.
//
// The program is intentionally simplistic but demonstrates
// the use of the FSEvents api in a hopefully clear fashion.
//
// To compile:
//    cc -I /System/Library/Frameworks/CoreServices.framework/Frameworks/CarbonCore.framework/Headers
//       -Wall -g -o watcher watcher.c -framework CoreServices -framework CoreFoundation
//
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/event.h>
#include <dirent.h>
#include <assert.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <pthread.h>

#include <sys/xattr.h>

typedef struct _settings_t {
    dev_t                dev;
    FSEventStreamEventId since_when;
    CFAbsoluteTime       latency;
    const char          *fullpath;
    CFUUIDRef            dev_uuid;
    char                 mount_point[MAXPATHLEN];
} settings_t;


//
// Prototypes
//
void  scan_directory(const char *path, int add, int recursive, int depth);
int   save_dir_items(const char *name);
int   load_dir_items(const char *name);
void  discard_all_dir_items(void);
int   remove_dir_and_children(const char *name);
int   check_children_of_dir(const char *dirname);
off_t get_total_size(void);

void  save_stream_info(uint64_t last_id, CFUUIDRef dev_uuid);
int   load_stream_info(uint64_t *since_when, CFUUIDRef *uuid_ref_ptr);

int   setup_run_loop_signal_handler(CFRunLoopRef loop);
void  cleanup_run_loop_signal_handler(CFRunLoopRef loop);

int   get_dev_info(settings_t *settings);
void  usage(const char *progname);
void  parse_settings(int argc, const char *argv[], settings_t *settings);
CFMutableArrayRef create_cfarray_from_path(const char *path);

void execute_for_path(const char *path);

//
//--------------------------------------------------------------------------------
// The FSEventsStreamCallback
//

static void
fsevents_callback(FSEventStreamRef streamRef, void *clientCallBackInfo,
                  int numEvents,
                  const char *const eventPaths[],
                  const FSEventStreamEventFlags *eventFlags,
                  const uint64_t *eventIDs)
{
    settings_t *settings = (settings_t *)clientCallBackInfo;
    const char *full_path = (const char *)settings->fullpath;
    int         i, len, recursive = 0;
    char        path_buff[PATH_MAX];


    for (i=0; i < numEvents; i++) {
	
	//
	// First, make a copy of the event path so we can modify it.
	//
	strcpy(path_buff, eventPaths[i]);
	len = strlen(path_buff);
	if (path_buff[len-1] == '/') {
	    // chop off a trailing slash so that scan_directory() works
	    path_buff[--len] = '\0';
	}

	//
	// Now check the flags for this event to see if we have to
	// do anything special.
	//
	// If we get a HistoryDone event we can just skip it.
	//
	// If we get a RootChanged event, check if the root exists
	// or not.  If not, throw away our state.  If it does, then
	// rebuild our state.
	//
	// Then of course if the MustScanSubDirs flag is set we
	// have to do a recursive scan to update our state.
	//
	if (eventFlags[i] & kFSEventStreamEventFlagHistoryDone) {
	    printf("Done processing historical events.  Current total size is: %lld for path: %s\n", get_total_size(), settings->fullpath);
	    continue;
	} else if (eventFlags[i] & kFSEventStreamEventFlagRootChanged) {
	    struct stat st;
	    
	    if (stat(full_path, &st) == 0) {
		printf("Root path %s now exists!\n", full_path);
		recursive = 1;
	    } else {
		printf("Root path %s no longer exists!\nNew total size: 0\n", full_path);
		discard_all_dir_items();
		continue;
	    }

	} else if (eventFlags[i] & kFSEventStreamEventFlagMustScanSubDirs) {
	    recursive = 1;

	    if (eventFlags[i] & kFSEventStreamEventFlagUserDropped) {
		printf("BAD NEWS! We dropped events.\n");
		strlcpy(path_buff, full_path, sizeof(path_buff));
	    } else if (eventFlags[i] & kFSEventStreamEventFlagKernelDropped) {
		printf("REALLY BAD NEWS! The kernel dropped events.\n");
		strlcpy(path_buff, full_path, sizeof(path_buff));
	    }
	} else {
	    recursive = 0;
	}

	//
	// Now go update our state and print out the new size
	// of the directory hierarchy we're watching.
	//
	if (recursive) {
	    remove_dir_and_children(path_buff);
	    scan_directory(path_buff, 1, 1, 0);
	} else {
	    check_children_of_dir(path_buff);
	}
//	printf("New total size: %lld (change made to: %s) for path: %s\n",
//		get_total_size(), path_buff, full_path);
    }
}



static void
watch_dir_hierarchy(settings_t *settings)
{
    FSEventStreamContext  context = {0, NULL, NULL, NULL, NULL};
    FSEventStreamRef      stream_ref = NULL;
    CFMutableArrayRef     cfarray_of_paths;
    int                   need_initial_scan = 0;
    CFUUIDRef             uuid_ref;

    //
    // Figure out the device of the path we're watching and
    // get its FSEventStream UUID.
    //
    if (get_dev_info(settings) != 0) {
	return;
    }


    //
    // Try to load the stored stream info
    //
    if (load_stream_info(&settings->since_when, &uuid_ref) == 0) {
	//
	// If we loaded the stream info cleanly, check if the uuid's match.
	//
	if (CFEqual(uuid_ref, settings->dev_uuid)) {
	    //
	    // The uuid's matched so now load the stored state for
	    // this hierarchy
	    //
	    if (load_dir_items("diritems.txt") == 0) {
		printf("Stored total size is: %lld for path: %s (since_when %lld)\n",
		    get_total_size(), settings->fullpath, settings->since_when);
	    } else {
		settings->since_when = kFSEventStreamEventIdSinceNow;
		need_initial_scan = 1;
	    }
	} else {
	    printf("UUID mis-match!  Ditching stored history id %lld\n", settings->since_when);
	    settings->since_when = kFSEventStreamEventIdSinceNow;
	    need_initial_scan = 1;
	}

	CFRelease(uuid_ref);

    } else {
	need_initial_scan = 1;
    }

    cfarray_of_paths = create_cfarray_from_path(settings->fullpath);
    if (cfarray_of_paths == NULL) {
	printf("failed to create the array for: %s\n", settings->fullpath);
	CFRelease(settings->dev_uuid);
	settings->dev_uuid = NULL;
	return;
    }

    context.info = (void *)settings;
    stream_ref = FSEventStreamCreate(kCFAllocatorDefault,
	                            (FSEventStreamCallback)&fsevents_callback,
	                            &context,
	                            cfarray_of_paths,
	                            settings->since_when,
	                            settings->latency,
	                            kFSEventStreamCreateFlagNone);
//	                            kFSEventStreamCreateFlagWatchRoot);

    CFRelease(cfarray_of_paths);
    if (stream_ref == NULL) {
	printf("failed to create the stream for: %s\n", settings->fullpath);
	CFRelease(settings->dev_uuid);
	settings->dev_uuid = NULL;
	return;
    }

    setup_run_loop_signal_handler(CFRunLoopGetCurrent());

    FSEventStreamScheduleWithRunLoop(stream_ref, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

    if (!FSEventStreamStart(stream_ref)) {
	fprintf(stderr, "failed to start the FSEventStream\n");
	goto out;
    }

    if (need_initial_scan) {
	//
	// NOTE: we get the initial size *after* we start the
	//       FSEventStream so that there is no window
	//       during which we would miss events.
	//
	scan_directory(settings->fullpath, 1, 1, 0);
	printf("Initial total size is: %lld for path: %s\n", get_total_size(), settings->fullpath);
    }


    //
    // Run
    //
    CFRunLoopRun();

    // Although it's not strictly necessary, make sure we see any pending events... 
    FSEventStreamFlushSync(stream_ref);
    
    FSEventStreamStop(stream_ref);

  out:

    //
    // Save out information about the last event id and uuid for the
    // the device we're watching.
    //
    save_stream_info(FSEventStreamGetLatestEventId(stream_ref), settings->dev_uuid);

    //
    // Save the directory item state
    //
    save_dir_items("diritems.txt");

    //
    // Invalidation and final shutdown of the stream
    //
    FSEventStreamInvalidate(stream_ref);
    FSEventStreamRelease(stream_ref);

    CFRelease(settings->dev_uuid);
    settings->dev_uuid = NULL;

    cleanup_run_loop_signal_handler(CFRunLoopGetCurrent());

    return;
}

//
//--------------------------------------------------------------------------------
//

int
main(int argc, const char * argv[])
{
    char       fullpath[PATH_MAX];
    settings_t _settings, *settings = &_settings;

    parse_settings(argc, argv, settings);
    
    if (settings->fullpath == NULL) {
	// no path given to monitor!
        usage(argv[0]);
    }
    
    if (realpath(settings->fullpath, fullpath) == NULL) {
	if (settings->fullpath[0] != '/') {
	    int len;
	    
	    getcwd(fullpath, sizeof(fullpath));
	    len = strlen(fullpath);
	    fullpath[len] = '/';
	    strlcpy(&fullpath[len+1], settings->fullpath, sizeof(fullpath) - (len+1));
	} else {
	    strlcpy(fullpath, settings->fullpath, sizeof(fullpath));
	}
    }

    settings->fullpath = fullpath;

    watch_dir_hierarchy(settings);
    
    return 0;
}


//
//--------------------------------------------------------------------------------
//

int
get_dev_info(settings_t *settings)
{
    struct stat st;
    dev_t       dev = 0;
    struct statfs sfs;
    char        path[MAXPATHLEN];

    settings->dev = 0;
    settings->mount_point[0] = '\0';

    strlcpy(path, settings->fullpath, sizeof(path));

    do {
	if (lstat(path, &st) == 0) {
	    dev = st.st_dev;
	} else {
	    char *ptr;

	    ptr = strrchr(path, '/');
	    if (ptr) {
		*ptr = '\0';
	    } else {
		path[0] = '\0';
	    }
	}
	
    } while(dev == 0 && path[0] != '\0');

    if (path[0] == '\0') {
	return -1;
    }

    settings->dev = dev;
    settings->dev_uuid = FSEventsCopyUUIDForDevice(settings->dev);
    if (settings->dev_uuid == NULL) {
	return -1;
    }

    if (statfs(path, &sfs) == 0) {
	strlcpy(settings->mount_point, sfs.f_mntonname, sizeof(settings->mount_point));
    } else {
	CFRelease(settings->dev_uuid);
	settings->dev_uuid = NULL;
	return -1;
    }

    return 0;
}


//
//--------------------------------------------------------------------------------
//

#define STREAM_INFO_NAME  "stream-info.txt"

void
save_stream_info(uint64_t last_id, CFUUIDRef uuid_ref)
{
    FILE *fp;
    CFStringRef cfstr;
    char uuid_str[64];

    fp = fopen(STREAM_INFO_NAME, "w");
    if (fp) {
	if (last_id == kFSEventStreamEventIdSinceNow || last_id == 0) {
	    last_id = FSEventsGetCurrentEventId();
	}
	printf("saving state: last_id %lld\n", last_id);

	fprintf(fp, "%lld\n", last_id);

	cfstr = CFUUIDCreateString(NULL, uuid_ref);
	if (cfstr && CFStringGetCString(cfstr, uuid_str, sizeof(uuid_str), kCFStringEncodingUTF8)) {
	    fprintf(fp, "%s\n", uuid_str);
	} else {
	    fprintf(fp, "unknown-uuid\n");
	}
	fclose(fp);

	if (cfstr) {
	    CFRelease(cfstr);
	}
    }
}


int
load_stream_info(uint64_t *since_when, CFUUIDRef *uuid_ref_ptr)
{
    FILE *fp;
    char uuid_str[64];
    CFStringRef cfstr;
    int ret=ENOMEM;

    fp = fopen(STREAM_INFO_NAME, "r");
    if (fp == NULL) {
	return ENOENT;
    }
    
    if (fscanf(fp, "%lld\n", since_when) != 1) {
	printf("error getting last id.\n");
	*since_when = kFSEventStreamEventIdSinceNow;
    }

    fscanf(fp, "%s\n", uuid_str);
    cfstr = CFStringCreateWithCString(NULL, uuid_str, kCFStringEncodingUTF8);
    if (cfstr) {
	*uuid_ref_ptr = CFUUIDCreateFromString(NULL, cfstr);
	if (*uuid_ref_ptr == NULL) {
	    printf("failed to create the dev uuid from str: %s\n", uuid_str);
	    *since_when = kFSEventStreamEventIdSinceNow;
	    ret = EINVAL;
	} else {
	    ret = 0;
	}
	
	CFRelease(cfstr);
    }
    fclose(fp);

    return ret;
}

//
//--------------------------------------------------------------------------------
//

void usage(const char *progname)
{
    printf("\n");
    printf("Usage: %s <options> <path>\n", progname);
    printf("Options:\n");
    printf("       -sinceWhen <when>          Specify a time from whence to search for applicable events\n");
    printf("       -latency <seconds>         Specify latency\n");
    printf("\n");
    exit(-1);
}


void
parse_settings(int argc, const char *argv[], settings_t *settings)
{
    int i;

    memset(settings, 0, sizeof(settings_t));

    settings->since_when = kFSEventStreamEventIdSinceNow;
    settings->latency = 0.5;

    for (i=1; i < argc; i++) {
        if (strcmp(argv[i], "-usage") == 0) {
            usage(argv[0]);
        } else if (strcmp(argv[i], "-since_when") == 0) {
            settings->since_when = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-latency") == 0) {
            settings->latency = strtod(argv[++i], NULL);
        } else {
            // Done parsing flags, the rest of the arguments must be paths.
            break;
        }
    }

    if (i < argc) {
        settings->fullpath = argv[i];
    } else {
	settings->fullpath = NULL;
    }
}


//
//--------------------------------------------------------------------------------
//  Simple wrapper to create a CFArray that contains a single
//  CFString in it (in this program it's the path we want to
//  watch).
//
//

CFMutableArrayRef
create_cfarray_from_path(const char *path)
{
    CFMutableArrayRef cfArray;

    cfArray = CFArrayCreateMutable(kCFAllocatorDefault, 1, &kCFTypeArrayCallBacks);
    if (cfArray == NULL) {
	fprintf(stderr, "%s: ERROR: CFArrayCreateMutable() => NULL\n", __FUNCTION__);
	return NULL;
    }

    CFStringRef cfStr = CFStringCreateWithCString(kCFAllocatorDefault, path, kCFStringEncodingUTF8);
    if (cfStr == NULL) {
	CFRelease(cfArray);
	return NULL;
    }

    CFArraySetValueAtIndex(cfArray, 0, cfStr);
    CFRelease(cfStr);
 	
    return cfArray;
}


//
//--------------------------------------------------------------------------------
// Routines to keep track of the size of the directory hierarchy 
// we are watching.
//
// This code is not exemplary in any way.  It works but is
// probably not the best way to keep track of directory
// state.
//

typedef struct dir_item {
    char       *dirname;
    short int   depth;
    short int   state;
    off_t       size;
} dir_item;

dir_item *dir_items=NULL;
int       num_dir_items=0;
int       max_dir_items=0;

#define DIR_ITEM_INCR  128

static int
compare_dir_items(const void *_a, const void *_b)
{
    dir_item *a = (dir_item *)_a, *b = (dir_item *)_b;

    if (a->dirname == NULL) {
	if (b->dirname == NULL) {
	    return 0;
	}
	return 1;
    } else if (b->dirname == NULL) {
	return -1;
    }

    return strcmp(a->dirname, b->dirname);
}


static int
add_dir_item(const char *name, off_t size, int depth)
{
    if (num_dir_items+1 >= max_dir_items) {
	dir_item *new;

	new = (dir_item *)realloc(dir_items, (max_dir_items+DIR_ITEM_INCR)*sizeof(dir_item));
	if (new == NULL) {
	    return ENOSPC;
	}
	dir_items = new;
	max_dir_items += DIR_ITEM_INCR;
    }

    dir_items[num_dir_items].dirname = strdup(name);
    dir_items[num_dir_items].depth   = depth;
    dir_items[num_dir_items].state   = 0;
    dir_items[num_dir_items].size    = size;
    
    num_dir_items++;
    return 0;
}

void
discard_all_dir_items(void)
{
    int i;

    for(i=0; i < num_dir_items; i++) {
	if (dir_items[i].dirname) {
	    free(dir_items[i].dirname);
	    dir_items[i].dirname = NULL;
	}

	dir_items[i].size = 0;
    }

    num_dir_items = 0;
}


int
save_dir_items(const char *name)
{
    int i;
    FILE *fp;

    fp = fopen(name, "w");
    if (fp == NULL) {
	printf("can't create %s\n", name);
	return -1;
    }

    for(i=0; i < num_dir_items; i++) {
	if (dir_items[i].dirname) {
	    fprintf(fp, "%d %lld %s\n", (int)dir_items[i].depth, dir_items[i].size, dir_items[i].dirname);
	}
    }

    fclose(fp);
    return 0;
}

int
load_dir_items(const char *name)
{
    FILE *fp;
    char  buff[MAXPATHLEN];
    int   depth;
    off_t size;

    fp = fopen(name, "r");
    if (fp == NULL) {
	printf("can't read %s\n", name);
	return -1;
    }

    while(fscanf(fp, "%d %lld %s\n", &depth, &size, buff) == 3) {
	add_dir_item(buff, size, depth);
    }

    fclose(fp);
    return 0;
}



static int
update_dir_item(const char *name, off_t size)
{
    int i;

    for(i=0; i < num_dir_items; i++) {
	if (dir_items[i].dirname != NULL && strcmp(name, dir_items[i].dirname) == 0) {
	    dir_items[i].size = size;
	    return 0;
	}
    }

    return ENOENT;
}


static void
cleanup_dir_items(void)
{
    int i;
    
    //
    // resort the list and then cleanup any dead guys.
    //
    qsort(dir_items, num_dir_items, sizeof(dir_item), compare_dir_items);

    for(i=num_dir_items-1; i >= 0; i--) {
	if (dir_items[i].dirname == NULL) {
	    num_dir_items--;
	}
    }
}


int
remove_dir_and_children(const char *name)
{
    int i, start_depth;

    for(i=0; i < num_dir_items; i++) {
	if (dir_items[i].dirname != NULL && strcmp(name, dir_items[i].dirname) == 0) {
	    break;
	}
    }

    if (i >= num_dir_items) {
	return ENOENT;
    }

    start_depth = dir_items[i].depth;
    
    for(; i < num_dir_items; i++) {
	if (dir_items[i].depth <= start_depth) {
	    break;
	}

	if (dir_items[i].dirname) {
	    free(dir_items[i].dirname);
	    dir_items[i].dirname = NULL;
	}

	dir_items[i].size = 0;
    }

    cleanup_dir_items();
    return 0;
}


// note: this returns zero if the directory exists
//       otherwise it returns one (i.e. true).
static int
dir_does_not_exist(const char *name)
{
    int i;

    for(i=0; i < num_dir_items; i++) {
	if (dir_items[i].dirname != NULL && strcmp(name, dir_items[i].dirname) == 0 && dir_items[i].size != 0) {
	    return 0;
	}
    }

    return 1;
}


// note: this returns zero if the directory did not 
//       exist in the list
static int
get_dir_depth(const char *name)
{
    int i;

    for(i=0; i < num_dir_items; i++) {
	if (dir_items[i].dirname != NULL && strcmp(name, dir_items[i].dirname) == 0) {
	    return dir_items[i].depth;
	}
    }

    return 0;
}


off_t
get_total_size(void)
{
    int   i;
    off_t size=0;

    for(i=0; i < num_dir_items; i++) {
	if (dir_items[i].dirname) {
	    size += dir_items[i].size;
	}
    }

    return size;
}


static off_t
iterate_subdirs(const char *dirname, int add, int recursive, int depth)
{
    char          *fullpath;
    DIR           *dir;
    struct dirent *dirent;
    struct stat    st;
    off_t          size=0, result=0;
    
    fullpath = malloc(PATH_MAX);
    if (fullpath == NULL) {
	return -1;
    }
    
    if (add) {
	add_dir_item(dirname, 0, depth);
    }

    if (depth == 0) {
	depth = get_dir_depth(dirname);
    }

    dir = opendir(dirname);
    if (dir == NULL) {
	if (errno == ENOENT) {             // it may have been deleted.
	    update_dir_item(dirname, 0);
	    return 0;
	}

	printf("failed to opendir(%s) (%s)\n", dirname, strerror(errno));
	return -1;
    }

    dirent = NULL;
    while ((dirent = readdir(dir)) != NULL) {
	if (strcmp(dirent->d_name, ".") == 0 || strcmp(dirent->d_name, "..") == 0)
	    continue;

	snprintf(fullpath, PATH_MAX, "%s/%s", dirname, dirent->d_name);
	if (lstat(fullpath, &st) != 0) {
	    printf("Error stating %s : %s\n", fullpath, strerror(errno));
	    continue;
	}
	
	execute_for_path(fullpath);

	size += st.st_size;
	
	if (S_ISDIR(st.st_mode) && (recursive || dir_does_not_exist(fullpath))) {
	    result = iterate_subdirs(fullpath, add, 1, depth+1);
	    if (result < 0) {
		printf("error getting size for %s\n", fullpath);
	    }
	}
    }

    closedir(dir);
    free(fullpath);

    if (update_dir_item(dirname, size) == ENOENT) {
	add_dir_item(dirname, size, depth);
    }

    return size;
}


int
check_children_of_dir(const char *dirname)
{
    int            i, current_depth, start_idx, end_idx;
    struct stat    st;
    off_t          dir_size;
    DIR           *dir;
    struct dirent *dirent;

    for(i=0; i < num_dir_items; i++) {
	if (dir_items[i].dirname != NULL && strcmp(dirname, dir_items[i].dirname) == 0) {
	    break;
	}
    }

    if (i >= num_dir_items) {
	return -1;
    }

    current_depth = dir_items[i].depth;
    start_idx = i;
    i++;
    for(; i < num_dir_items; i++) {
	if (dir_items[i].depth <= current_depth) {
	    break;
	}
    }
    end_idx = i;
    
    dir = opendir(dirname);
    if (dir == NULL) {
	if (errno == ENOENT) {
	    for(i=start_idx; i < end_idx; i++) {
		dir_items[i].size = 0;
	    }

	    return 0;
	}	    
    }

    dir_size = 0;
    while((dirent = readdir(dir)) != NULL) {
	char fullpath[MAXPATHLEN];
	
	if (strcmp(dirent->d_name, ".") == 0 || strcmp(dirent->d_name, "..") == 0)
	    continue;

	snprintf(fullpath, MAXPATHLEN, "%s/%s", dirname, dirent->d_name);
	for(i=start_idx; i < end_idx; i++) {
	    if (dir_items[i].dirname == NULL) {
		continue;
	    }

	    if (dir_items[i].depth == current_depth+1 && strcmp(dir_items[i].dirname, fullpath) == 0) {
		// flag that it exists
		dir_items[i].state = 1;
		break;
	    }
	}

	if (lstat(fullpath, &st) == 0) {
	    if (S_ISDIR(st.st_mode)) {
		if (i >= end_idx) {
		    // printf("NEW item: %s\n", fullpath);
		    iterate_subdirs(fullpath, 1, 1, current_depth+1);
		}
		dir_size += st.st_size;
	    } else {
		dir_size += st.st_size;
	    }
	    execute_for_path(fullpath);
	}
    }

    i = start_idx;
    dir_items[i].size = dir_size;

    for(i=start_idx; i < end_idx; i++) {
	if (dir_items[i].depth == current_depth+1) {
	    if (dir_items[i].dirname != NULL && dir_items[i].state == 0) {
		int j;
		
		// printf("DELETED item: %s\n", dir_items[i].dirname);
		// go through and clear out that directory and all of its
		// children.
		for(j=i; j < end_idx; j++) {
		    if (j > i && dir_items[j].depth <= current_depth+1) {
			break;
		    } else if (dir_items[j].dirname) {
			free(dir_items[j].dirname);
			dir_items[j].dirname = NULL;
			dir_items[j].size = 0;
		    }
		}
	    } else {
		dir_items[i].state = 0;
	    }
	}
    }

    cleanup_dir_items();
    
//    for(i=0; i < num_dir_items; i++) {
//	printf("%d %s\n", dir_items[i].depth, dir_items[i].dirname);
//    }
//    printf("--------------------------------------------\n");

    return 0;
}


void
scan_directory(const char *dirname, int add, int recursive, int depth)
{
    iterate_subdirs(dirname, add, recursive, depth);

    qsort(dir_items, num_dir_items, sizeof(dir_item), compare_dir_items);
}


//
// ----------------- run loop signal handling stuff ---------------------
//
// This code provides a way for a command-line CFRunLoop based
// application to exit cleanly when you press ^C or get sent a
// signal like SIGHUP.
//
// It sets up signal handlers and then creates a kqueue to
// watch for signal events.  The kqueue is then plugged into
// a CFFileDescriptorRef which calls a callback when there
// is activity on the kqueue.  The callback stops the run
// loop and that lets everything shuts down cleanly.
//
CFFileDescriptorRef   kq_cffd   = NULL;
CFRunLoopSourceRef    kq_rl_src = NULL;
int                   kq_fd     = -1;

static void
sig_handler(int sig)
{
    // there's nothing to do in the signal handler...
    // the real work happens in kq_cffd_callback where 
    // we can safely call CFRunLoopStop()
}


static void
kq_cffd_callback(CFFileDescriptorRef kq_cffd, CFOptionFlags callBackTypes, void *info)
{
    CFRunLoopRef loop = (CFRunLoopRef)info;

    CFRunLoopStop(loop);
}


int
setup_run_loop_signal_handler(CFRunLoopRef loop)
{
    CFFileDescriptorContext my_context;
    struct kevent           kev[4];

    signal(SIGINT,  sig_handler);
    signal(SIGQUIT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGHUP,  sig_handler);

    kq_fd = kqueue();
    if (kq_fd < 0) {
	return -1;
    }

    EV_SET(&kev[0], SIGINT,  EVFILT_SIGNAL, EV_ADD, 0, 0, 0);
    EV_SET(&kev[1], SIGQUIT, EVFILT_SIGNAL, EV_ADD, 0, 0, 0);
    EV_SET(&kev[2], SIGTERM, EVFILT_SIGNAL, EV_ADD, 0, 0, 0);
    EV_SET(&kev[3], SIGHUP,  EVFILT_SIGNAL, EV_ADD, 0, 0, 0);

    if (kevent(kq_fd, &kev[0], 4, NULL, 0, NULL) != 0) {
	close(kq_fd);
	return -1;
    }
    
    memset(&my_context, 0, sizeof(CFFileDescriptorContext));
    my_context.info = (void *)loop;

    kq_cffd = CFFileDescriptorCreate(NULL, kq_fd, 1, kq_cffd_callback, &my_context);
    if (kq_cffd == NULL) {
	close(kq_fd);
	return -1;
    }

    kq_rl_src = CFFileDescriptorCreateRunLoopSource(NULL, kq_cffd, (CFIndex)0);
    if (kq_rl_src == NULL) {
	// Dispose the kq_cffd
	CFFileDescriptorInvalidate(kq_cffd);
	CFRelease(kq_cffd);
	kq_cffd = NULL;

	return -1;
    }

    CFRunLoopAddSource(loop, kq_rl_src, kCFRunLoopDefaultMode);
    CFFileDescriptorEnableCallBacks(kq_cffd, kCFFileDescriptorReadCallBack);

    return 0;
}


void
cleanup_run_loop_signal_handler(CFRunLoopRef loop)
{
    CFRunLoopRemoveSource(loop, kq_rl_src, kCFRunLoopDefaultMode);

    CFFileDescriptorInvalidate(kq_cffd);
    CFRelease(kq_rl_src);
    CFRelease(kq_cffd);
    close(kq_fd);

    kq_rl_src = NULL;
    kq_cffd   = NULL;
    kq_fd     = -1;
}

void execute_for_path(const char *path)
{
    ssize_t result = getxattr(path, "net_sourceforge_skim-app_notes", NULL, 0, 0, 0);
    if (result > 0) {
	printf("Will convert notes for: %s\n", path);
	char *buffer = (char*)malloc(PATH_MAX);
	snprintf(buffer, PATH_MAX, "/Applications/Skim.app/Contents/SharedSupport/skimnotes get \"%s\"", path);
	system(buffer);
	snprintf(buffer, PATH_MAX, "/Applications/Skim.app/Contents/SharedSupport/skimnotes remove \"%s\"", path);
	system(buffer);
    }
}

