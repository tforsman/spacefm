/*
*  C Interface: vfs-file-task
*
* Description:
*
*
* Author: Hong Jen Yee (PCMan) <pcman.tw (AT) gmail.com>, (C) 2005
*
* Copyright: See COPYING file that comes with this distribution
*
*/
#ifndef  _VFS_FILE_TASK_H
#define  _VFS_FILE_TASK_H

#include <glib.h>
#include <sys/types.h>
#include <gtk/gtk.h>

typedef enum
{
    VFS_FILE_TASK_MOVE = 0,
    VFS_FILE_TASK_COPY,
    VFS_FILE_TASK_TRASH,
    VFS_FILE_TASK_DELETE,
    VFS_FILE_TASK_LINK,             /* will be supported in the future */
    VFS_FILE_TASK_CHMOD_CHOWN,         /* These two kinds of operation have lots in common,
                                        * so put them together to reduce duplicated disk I/O */
    VFS_FILE_TASK_EXEC,             //MOD
    VFS_FILE_TASK_LAST
}VFSFileTaskType;

typedef enum {
    OWNER_R = 0,
    OWNER_W,
    OWNER_X,
    GROUP_R,
    GROUP_W,
    GROUP_X,
    OTHER_R,
    OTHER_W,
    OTHER_X,
    SET_UID,
    SET_GID,
    STICKY,
    N_CHMOD_ACTIONS
}ChmodActionType;

extern const mode_t chmod_flags[];

struct _VFSFileTask;

typedef enum
{
    VFS_FILE_TASK_RUNNING,
    VFS_FILE_TASK_QUERY_ABORT,
    VFS_FILE_TASK_QUERY_OVERWRITE,
    VFS_FILE_TASK_ERROR,
    VFS_FILE_TASK_ABORTED,
    VFS_FILE_TASK_FINISH
}VFSFileTaskState;

typedef enum
{
    VFS_FILE_TASK_OVERWRITE, /* Overwrite current dest file */
    VFS_FILE_TASK_OVERWRITE_ALL, /* Overwrite all existing files without prompt */
    VFS_FILE_TASK_SKIP, /* Don't overwrite current file */
    VFS_FILE_TASK_SKIP_ALL, /* Don't try to overwrite any files */
    VFS_FILE_TASK_RENAME /* Rename file */
}VFSFileTaskOverwriteMode;

typedef enum
{
    VFS_EXEC_NORMAL,
    VFS_EXEC_CUSTOM,
    VFS_EXEC_UDISKS
}VFSExecType;


typedef struct _VFSFileTask VFSFileTask;

typedef void ( *VFSFileTaskProgressCallback ) ( VFSFileTask* task,
                                                int percent,
                                                const char* src_file,
                                                const char* dest_file,
                                                gpointer user_data );

typedef gboolean ( *VFSFileTaskStateCallback ) ( VFSFileTask*,
                                                 VFSFileTaskState state,
                                                 gpointer state_data,
                                                 gpointer user_data );

struct _VFSFileTask
{
    VFSFileTaskType type;
    GList* src_paths; /* All source files. This list will be freed
                                 after file operation is completed. */
    char* dest_dir; /* Destinaton directory */

    VFSFileTaskOverwriteMode overwrite_mode ;
    gboolean recursive; /* Apply operation to all files under folders
        * recursively. This is default to copy/delete,
        * and should be set manually for chown/chmod. */

    /* For chown */
    uid_t uid;
    gid_t gid;

    /* For chmod */
    guchar *chmod_actions;  /* If chmod is not needed, this should be NULL */

    off64_t total_size; /* Total size of the files to be processed, in bytes */
    off64_t progress; /* Total size of current processed files, in btytes */
    int percent; /* progress (percentage) */
    time_t start_time;  //MOD
    time_t last_time;   //MOD
    time_t update_time;   //MOD
    off64_t current_speed;   //MOD
    off64_t current_progress; //MOD
    guint current_item;  //MOD
    guint ticks;
    guint flood_ticks;
    int err_count;  //MOD
    char* err_msgs;
    
    const char* current_file; /* Current processed file */
    const char* current_dest; /* Current destination file */

    int error;

    GThread* thread;
    VFSFileTaskState state;

    VFSFileTaskProgressCallback progress_cb;
    gpointer progress_cb_data;

    VFSFileTaskStateCallback state_cb;
    gpointer state_cb_data;
    
    //MOD run task
    VFSExecType exec_type;
    char* exec_action;
    char* exec_command;
    gboolean exec_sync;
    gboolean exec_popup;
    gboolean exec_show_output;
    gboolean exec_show_error;
    gboolean exec_terminal;
    gboolean exec_keep_terminal;
    gboolean exec_export;
    gboolean exec_direct;
    char* exec_argv[7];      // for exec_direct, command ignored
                             // for su commands, must use /bin/bash -c
                             // as su does not execute binaries
    char* exec_script;
    gboolean exec_keep_tmp;  // diagnostic to keep temp files
    gpointer exec_browser;
    gpointer exec_desktop;
    char* exec_as_user;
    char* exec_icon;
    GPid exec_pid;
    int exec_exit_status;
    gboolean exec_is_error;
    GIOChannel* exec_channel_out;
    GIOChannel* exec_channel_err;
    GtkTextBuffer* exec_err_buf;  //copy from ptk task
    GtkTextMark* exec_mark_end;  //copy from ptk task
    gboolean exec_scroll_lock;
    gboolean exec_write_root;
    gpointer exec_set;
};

/*
* source_files sould be a newly allocated list, and it will be
* freed after file operation has been completed
*/
VFSFileTask* vfs_task_new ( VFSFileTaskType task_type,
                            GList* src_files,
                            const char* dest_dir );

/* Set some actions for chmod, this array will be copied
* and stored in VFSFileTask */
void vfs_file_task_set_chmod( VFSFileTask* task,
                              guchar* chmod_actions );

void vfs_file_task_set_chown( VFSFileTask* task,
                              uid_t uid, gid_t gid );

void vfs_file_task_set_progress_callback( VFSFileTask* task,
                                          VFSFileTaskProgressCallback cb,
                                          gpointer user_data );

void vfs_file_task_set_state_callback( VFSFileTask* task,
                                       VFSFileTaskStateCallback cb,
                                       gpointer user_data );

void vfs_file_task_set_recursive( VFSFileTask* task, gboolean recursive );

void vfs_file_task_set_overwrite_mode( VFSFileTask* task,
                                       VFSFileTaskOverwriteMode mode );

void vfs_file_task_run ( VFSFileTask* task );


void vfs_file_task_try_abort ( VFSFileTask* task );

void vfs_file_task_abort ( VFSFileTask* task );

void vfs_file_task_free ( VFSFileTask* task );

char* vfs_file_task_get_cpids( GPid pid );
void vfs_file_task_kill_cpids( char* cpids, int signal );

#endif
