/*
*
* Copyright: See COPYING file that comes with this distribution
*
*/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

/* turn on to debug GDK_THREADS_ENTER/GDK_THREADS_LEAVE related deadlocks */
#undef _DEBUG_THREAD

#include "private.h"

#include <gtk/gtk.h>
#include <glib.h>

#include <stdlib.h>
#include <string.h>

/* socket is used to keep single instance */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <signal.h>

#include <unistd.h> /* for getcwd */

#include "main-window.h"

#include "vfs-file-info.h"
#include "vfs-mime-type.h"
#include "vfs-app-desktop.h"

#include "vfs-file-monitor.h"
#include "vfs-volume.h"
#include "vfs-thumbnail-loader.h"

#include "ptk-utils.h"
#include "ptk-app-chooser.h"
#include "ptk-file-properties.h"
#include "ptk-file-menu.h"

#include "find-files.h"
#include "pref-dialog.h"
#include "settings.h"

#include "desktop.h"

//gboolean startup_mode = TRUE;  //MOD
//gboolean design_mode = TRUE;  //MOD

char* run_cmd = NULL;  //MOD

typedef enum{
    CMD_OPEN = 1,
    CMD_OPEN_TAB,
    CMD_REUSE_TAB,
    CMD_DAEMON_MODE,
    CMD_PREF,
    CMD_WALLPAPER,
    CMD_FIND_FILES,
    CMD_OPEN_PANEL1,
    CMD_OPEN_PANEL2,
    CMD_OPEN_PANEL3,
    CMD_OPEN_PANEL4,
    CMD_PANEL1,
    CMD_PANEL2,
    CMD_PANEL3,
    CMD_PANEL4,
    CMD_DESKTOP
}SocketEvent;

static gboolean folder_initialized = FALSE;
static gboolean desktop_or_deamon_initialized = FALSE;

static int sock;
GIOChannel* io_channel = NULL;

gboolean daemon_mode = FALSE;

static char* default_files[2] = {NULL, NULL};
static char** files = NULL;
static gboolean no_desktop = FALSE;
static gboolean old_show_desktop = FALSE;

static gboolean new_tab = TRUE;
static gboolean reuse_tab = FALSE;  //sfm
static gboolean new_window = FALSE;
static gboolean desktop_pref = FALSE;  //MOD
static gboolean desktop = FALSE;  //MOD
static gboolean profile = FALSE;  //MOD
static gboolean sdebug = FALSE;

static int show_pref = 0;
static int panel = -1;
static gboolean set_wallpaper = FALSE;

static gboolean find_files = FALSE;
static char* config_dir = NULL;

#ifdef HAVE_HAL
static char* mount = NULL;
static char* umount = NULL;
static char* eject = NULL;
#endif

static int n_pcmanfm_ref = 0;

static GOptionEntry opt_entries[] =
{
    { "new-tab", 't', 0, G_OPTION_ARG_NONE, &new_tab, N_("Open folders in new tab of last window (default)"), NULL },
    { "reuse-tab", 'r', 0, G_OPTION_ARG_NONE, &reuse_tab, N_("Open folder in current tab of last used window"), NULL },
    { "new-window", 'w', 0, G_OPTION_ARG_NONE, &new_window, N_("Open folders in new window"), NULL },
    { "panel", 'p', 0, G_OPTION_ARG_INT, &panel, N_("Open folders in panel 'P' (1-4)"), "P" },
    { "desktop", '\0', 0, G_OPTION_ARG_NONE, &desktop, N_("Launch desktop manager"), NULL },
    { "desktop-pref", '\0', 0, G_OPTION_ARG_NONE, &desktop_pref, N_("Show desktop settings"), NULL },
    { "show-pref", '\0', 0, G_OPTION_ARG_INT, &show_pref, N_("Show Preferences ('N' is the Pref tab number)"), "N" },
    { "daemon-mode", 'd', 0, G_OPTION_ARG_NONE, &daemon_mode, N_("Run as a daemon"), NULL },
    { "config-dir", 'c', 0, G_OPTION_ARG_STRING, &config_dir, N_("Use DIR as configuration directory"), "DIR" },
    { "find-files", 'f', 0, G_OPTION_ARG_NONE, &find_files, N_("Show File Search"), NULL },
/*
    { "query-type", '\0', 0, G_OPTION_ARG_STRING, &query_type, N_("Query mime-type of the specified file."), NULL },
    { "query-default", '\0', 0, G_OPTION_ARG_STRING, &query_default, N_("Query default application of the specified mime-type."), NULL },
    { "set-default", '\0', 0, G_OPTION_ARG_STRING, &set_default, N_("Set default application of the specified mime-type."), NULL },
*/
#ifdef DESKTOP_INTEGRATION
    { "set-wallpaper", '\0', 0, G_OPTION_ARG_NONE, &set_wallpaper, N_("Set desktop wallpaper to FILE"), NULL },
#endif
    { "profile", '\0', 0, G_OPTION_ARG_STRING, &profile, N_("No function - for compatibility only"), "PROFILE" },
    { "no-desktop", '\0', 0, G_OPTION_ARG_NONE, &no_desktop, N_("No function - for compatibility only"), NULL },

    { "sdebug", '\0', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &sdebug, NULL, NULL },

#ifdef HAVE_HAL
    /* hidden arguments used to mount volumes */
    { "mount", 'm', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &mount, NULL, NULL },
    { "umount", 'u', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &umount, NULL, NULL },
    { "eject", 'e', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &eject, NULL, NULL },
#endif
    {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &files, NULL, N_("[DIR | FILE]...")},
    { NULL }
};

static gboolean single_instance_check();
static void single_instance_finalize();
static void get_socket_name( char* buf, int len );
static gboolean on_socket_event( GIOChannel* ioc, GIOCondition cond, gpointer data );

static void init_folder();
static void init_daemon_or_desktop();
static void check_icon_theme();

static gboolean handle_parsed_commandline_args();

static FMMainWindow* create_main_window();
static void open_file( const char* path );

static GList* get_file_info_list( char** files );
static char* dup_to_absolute_file_path( char** file );

gboolean on_socket_event( GIOChannel* ioc, GIOCondition cond, gpointer data )
{
    int client, r;
    socklen_t addr_len = 0;
    struct sockaddr_un client_addr ={ 0 };
    static char buf[ 1024 ];
    GString* args;
    char** file;
    SocketEvent cmd;

    if ( cond & G_IO_IN )
    {
        client = accept( g_io_channel_unix_get_fd( ioc ), (struct sockaddr *)&client_addr, &addr_len );
        if ( client != -1 )
        {
            args = g_string_new_len( NULL, 2048 );
            while( (r = read( client, buf, sizeof(buf) )) > 0 )
                g_string_append_len( args, buf, r);
            shutdown( client, 2 );
            close( client );

            new_tab = TRUE;

            switch( args->str[0] )
            {
            case CMD_REUSE_TAB:
                new_tab = FALSE;
                reuse_tab = TRUE;
                break;
            case CMD_PANEL1:
                panel = 1;
                break;
            case CMD_PANEL2:
                panel = 2;
                break;
            case CMD_PANEL3:
                panel = 3;
                break;
            case CMD_PANEL4:
                panel = 4;
                break;
            case CMD_OPEN:
                new_tab = FALSE;
                break;
            case CMD_OPEN_PANEL1:
                new_tab = FALSE;
                panel = 1;
                break;
            case CMD_OPEN_PANEL2:
                new_tab = FALSE;
                panel = 2;
                break;
            case CMD_OPEN_PANEL3:
                new_tab = FALSE;
                panel = 3;
                break;
            case CMD_OPEN_PANEL4:
                new_tab = FALSE;
                panel = 4;
                break;
            case CMD_DAEMON_MODE:
                daemon_mode = TRUE;
                g_string_free( args, TRUE );
                return TRUE;
            case CMD_DESKTOP:
                desktop = TRUE;
                break;
            case CMD_PREF:
                GDK_THREADS_ENTER();
                fm_edit_preference( NULL, (unsigned char)args->str[1] - 1 );
                GDK_THREADS_LEAVE();
                return TRUE;
            case CMD_WALLPAPER:
                set_wallpaper = TRUE;
                break;
            case CMD_FIND_FILES:
                find_files = TRUE;
                break;
            }

            if( args->str[ 1 ] )
                files = g_strsplit( args->str + 1, "\n", 0 );
            else
                files = NULL;
            g_string_free( args, TRUE );

            GDK_THREADS_ENTER();

            if( files )
            {
                for( file = files; *file; ++file )
                {
                    if( ! **file )  /* remove empty string at tail */
                        *file = NULL;
                }
            }
            handle_parsed_commandline_args();

            GDK_THREADS_LEAVE();
        }
    }

    return TRUE;
}

void get_socket_name( char* buf, int len )
{
    char* dpy = gdk_get_display();
    if ( dpy && !strcmp( dpy, ":0.0" ) )
    {
        // treat :0.0 as :0 to prevent multiple instances on screen 0
        g_free( dpy );
        dpy = g_strdup( ":0" );
    }
    g_snprintf( buf, len, "/tmp/.spacefm-socket%s-%s", dpy, g_get_user_name() );
    g_free( dpy );
}

gboolean single_instance_check()
{
    struct sockaddr_un addr;
    int addr_len;
    int ret;
    int reuse;

    if ( ( sock = socket( AF_UNIX, SOCK_STREAM, 0 ) ) == -1 )
    {
        ret = 1;
        goto _exit;
    }

    addr.sun_family = AF_UNIX;
    get_socket_name( addr.sun_path, sizeof( addr.sun_path ) );
#ifdef SUN_LEN
    addr_len = SUN_LEN( &addr );
#else

    addr_len = strlen( addr.sun_path ) + sizeof( addr.sun_family );
#endif

    /* try to connect to existing instance */
    if ( connect( sock, ( struct sockaddr* ) & addr, addr_len ) == 0 )
    {
        /* connected successfully */
        char** file;
        char cmd = CMD_OPEN_TAB;

        if( daemon_mode )
            cmd = CMD_DAEMON_MODE;
        else if( desktop )
            cmd = CMD_DESKTOP;
        else if( new_window )
        {
            if ( panel > 0 && panel < 5 )
                cmd = CMD_OPEN_PANEL1 + panel - 1;
            else
                cmd = CMD_OPEN;
        }
        else if ( reuse_tab )
            cmd = CMD_REUSE_TAB;
        else if( show_pref > 0 )
            cmd = CMD_PREF;
        else if ( desktop_pref )  //MOD
        {
            cmd = CMD_PREF;
            show_pref = 3;
        }
        else if( set_wallpaper )
            cmd = CMD_WALLPAPER;
        else if( find_files )
            cmd = CMD_FIND_FILES;
        else if ( panel > 0 && panel < 5 )
            cmd = CMD_PANEL1 + panel - 1;            

        // open a new window if no file spec
        if ( cmd == CMD_OPEN_TAB && !files )
            cmd = CMD_OPEN;
            
        write( sock, &cmd, sizeof(char) );
        if( G_UNLIKELY( show_pref > 0 ) )
        {
            cmd = (unsigned char)show_pref;
            write( sock, &cmd, sizeof(char) );
        }
        else
        {
            if( files )
            {
                for( file = files; *file; ++file )
                {
                    char *real_path;

                    /* We send absolute paths because with different
                       $PWDs resolution would not work. */
                    real_path = dup_to_absolute_file_path( file );
                    write( sock, real_path, strlen( real_path ) );
                    g_free( real_path );
                    write( sock, "\n", 1 );
                }
            }
        }
        if ( config_dir )
            g_warning( _("Option --config-dir ignored - an instance is already running") );
        shutdown( sock, 2 );
        close( sock );
        ret = 0;
        goto _exit;
    }

    /* There is no existing server, and we are in the first instance. */
    unlink( addr.sun_path ); /* delete old socket file if it exists. */
    reuse = 1;
    ret = setsockopt( sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    if ( bind( sock, ( struct sockaddr* ) & addr, addr_len ) == -1 )
    {
        ret = 1;
        goto _exit;
    }

    io_channel = g_io_channel_unix_new( sock );
    g_io_channel_set_encoding( io_channel, NULL, NULL );
    g_io_channel_set_buffered( io_channel, FALSE );

    g_io_add_watch( io_channel, G_IO_IN,
                    ( GIOFunc ) on_socket_event, NULL );

    if ( listen( sock, 5 ) == -1 )
    {
        ret = 1;
        goto _exit;
    }
    
    // custom config-dir
    if ( config_dir && strpbrk( config_dir, " $%\\()&#|:;?<>{}[]*\"'" ) )
    {
        g_warning( _("Option --config-dir contains invalid chars - cannot start") );
        ret = 1;
        goto _exit;
    }
    return TRUE;

_exit:

    gdk_notify_startup_complete();
    exit( ret );
}

void single_instance_finalize()
{
    char lock_file[ 256 ];

    shutdown( sock, 2 );
    g_io_channel_unref( io_channel );
    close( sock );

    get_socket_name( lock_file, sizeof( lock_file ) );
    unlink( lock_file );
}

FMMainWindow* create_main_window()
{
    FMMainWindow * main_window = FM_MAIN_WINDOW(fm_main_window_new ());
    gtk_window_set_default_size( GTK_WINDOW( main_window ),
                                 app_settings.width, app_settings.height );
    if ( app_settings.maximized )
    {
        gtk_window_maximize( GTK_WINDOW( main_window ) );
    }
    gtk_widget_show ( GTK_WIDGET( main_window ) );
    return main_window;
}
/*
void check_icon_theme()
{
    GtkSettings * settings;
    char* theme;
    const char* title = N_( "GTK+ icon theme is not properly set" );
    const char* error_msg =
        N_( "<big><b>%s</b></big>\n\n"
            "This usually means you don't have an XSETTINGS manager running.  "
            "Desktop environment like GNOME or XFCE automatically execute their "
            "XSETTING managers like gnome-settings-daemon or xfce-mcs-manager.\n\n"
            "<b>If you don't use these desktop environments, "
            "you have two choices:\n"
            "1. run an XSETTINGS manager, or\n"
            "2. simply specify an icon theme in ~/.gtkrc-2.0.</b>\n"
            "For example to use the Tango icon theme add a line:\n"
            "<i><b>gtk-icon-theme-name=\"Tango\"</b></i> in your ~/.gtkrc-2.0. (create it if no such file)\n\n"
            "<b>NOTICE: The icon theme you choose should be compatible with GNOME, "
            "or the file icons cannot be displayed correctly.</b>  "
            "Due to the differences in icon naming of GNOME and KDE, KDE themes cannot be used.  "
            "Currently there is no standard for this, but it will be solved by freedesktop.org in the future." );
    settings = gtk_settings_get_default();
    g_object_get( settings, "gtk-icon-theme-name", &theme, NULL );

    // No icon theme available
    if ( !theme || !*theme || 0 == strcmp( theme, "hicolor" ) )
    {
        GtkWidget * dlg;
        dlg = gtk_message_dialog_new_with_markup( NULL,
                                                  GTK_DIALOG_MODAL,
                                                  GTK_MESSAGE_ERROR,
                                                  GTK_BUTTONS_OK,
                                                  _( error_msg ), _( title ) );
        gtk_window_set_title( GTK_WINDOW( dlg ), _( title ) );
        gtk_dialog_run( GTK_DIALOG( dlg ) );
        gtk_widget_destroy( dlg );
    }
    g_free( theme );
}
*/
#ifdef _DEBUG_THREAD

G_LOCK_DEFINE(gdk_lock);
void debug_gdk_threads_enter (const char* message)
{
    g_debug( "Thread %p tries to get GDK lock: %s", g_thread_self (), message );
    G_LOCK(gdk_lock);
    g_debug( "Thread %p got GDK lock: %s", g_thread_self (), message );
}

static void _debug_gdk_threads_enter ()
{
    debug_gdk_threads_enter( "called from GTK+ internal" );
}

void debug_gdk_threads_leave( const char* message )
{
    g_debug( "Thread %p tries to release GDK lock: %s", g_thread_self (), message );
    G_LOCK(gdk_lock);
    g_debug( "Thread %p released GDK lock: %s", g_thread_self (), message );
}

static void _debug_gdk_threads_leave()
{
    debug_gdk_threads_leave( "called from GTK+ internal" );
}
#endif

void init_folder()
{
    if( G_LIKELY(folder_initialized) )
        return;

    app_settings.bookmarks = ptk_bookmarks_get();

    vfs_volume_init();
    vfs_thumbnail_init();

    vfs_mime_type_set_icon_size( app_settings.big_icon_size,
                                 app_settings.small_icon_size );
    vfs_file_info_set_thumbnail_size( app_settings.big_icon_size,
                                      app_settings.small_icon_size );

    //check_icon_theme();  //sfm seems to run okay without gtk theme
    folder_initialized = TRUE;
}

static void init_daemon_or_desktop()
{
    if( desktop )
        fm_turn_on_desktop_icons();
}

#ifdef HAVE_HAL

/* FIXME: Currently, this cannot be supported without HAL */

static int handle_mount( char** argv )
{
    gboolean success;
    vfs_volume_init();
    if( mount )
        success = vfs_volume_mount_by_udi( mount, NULL );
    else if( umount )
        success = vfs_volume_umount_by_udi( umount, NULL );
    else /* if( eject ) */
        success = vfs_volume_eject_by_udi( eject, NULL );
    vfs_volume_finalize();
    return success ? 0 : 1;
}
#endif

GList* get_file_info_list( char** file_paths )
{
    GList* file_list = NULL;
    char** file;
    VFSFileInfo* fi;

    for( file = file_paths; *file; ++file )
    {
        fi = vfs_file_info_new();
        if( vfs_file_info_get( fi, *file, NULL ) )
            file_list = g_list_append( file_list, fi );
        else
            vfs_file_info_unref( fi );
    }

    return file_list;
}

gboolean delayed_popup( GtkWidget* popup )
{
    GDK_THREADS_ENTER();

    gtk_menu_popup( GTK_MENU( popup ), NULL, NULL,
                    NULL, NULL, 0, gtk_get_current_event_time() );

    GDK_THREADS_LEAVE();

    return FALSE;
}

static void init_desktop_or_daemon()
{
    init_folder();

    signal( SIGPIPE, SIG_IGN );
    signal( SIGHUP, (void*)gtk_main_quit );
    signal( SIGINT, (void*)gtk_main_quit );
    signal( SIGTERM, (void*)gtk_main_quit );

    if( desktop )
        fm_turn_on_desktop_icons();
    desktop_or_deamon_initialized = TRUE;
}

char* dup_to_absolute_file_path(char** file)
{
    char* file_path, *real_path, *cwd_path;
    const size_t cwd_size = PATH_MAX;

    if( g_str_has_prefix( *file, "file:" ) ) /* It's a URI */
    {
        file_path = g_filename_from_uri( *file, NULL, NULL );
        g_free( *file );
        *file = file_path;
    }
    else
        file_path = *file;

    cwd_path = malloc( cwd_size );
    if( cwd_path )
    {
        getcwd( cwd_path, cwd_size );
    }

    real_path = vfs_file_resolve_path( cwd_path, file_path );
    free( cwd_path );
    cwd_path = NULL;

    return real_path; /* To free with g_free */
}

gboolean handle_parsed_commandline_args()
{
    FMMainWindow * main_window = NULL;
    char** file;
    gboolean ret = TRUE;
    XSet* set;
    gboolean tab_added;
    int p;
    
    // If no files are specified, open home dir by defualt.
    if( G_LIKELY( ! files ) )
    {
        files = default_files;
        //files[0] = (char*)g_get_home_dir();
    }

    /* get the last active window, if available */
    if( new_tab || reuse_tab )
    {
        main_window = fm_main_window_get_last_active();
    }

    if ( desktop_pref )  //MOD
        show_pref = 3;

    if( show_pref > 0 ) /* show preferences dialog */
    {
        /* We should initialize desktop support here.
         * Otherwise, if the user turn on the desktop support
         * in the pref dialog, the newly loaded desktop will be uninitialized.
         */
        //init_desktop_or_daemon();
        fm_edit_preference( GTK_WINDOW( main_window ), show_pref - 1 );
        show_pref = 0;
    }
    else if( find_files ) /* find files */
    {
        init_folder();
        fm_find_files( (const char**)files );
        find_files = FALSE;
    }
#ifdef DESKTOP_INTEGRATION
    else if( set_wallpaper ) /* change wallpaper */
    {
        char* file = files ? files[0] : NULL;
        char* path;
        if( ! file )
            return FALSE;

        if( g_str_has_prefix( file, "file:" ) )  /* URI */
        {
            path = g_filename_from_uri( file, NULL, NULL );
            g_free( file );
            file = path;
        }
        else
            file = g_strdup( file );

        if( g_file_test( file, G_FILE_TEST_IS_REGULAR ) )
        {
            g_free( app_settings.wallpaper );
            app_settings.wallpaper = file;
            char* err_msg = save_settings( NULL );
            if ( err_msg )
                printf( _("spacefm: Error: Unable to save session\n       %s\n"), err_msg );

            if( desktop && app_settings.show_wallpaper )
            {
                if( desktop_or_deamon_initialized )
                    fm_desktop_update_wallpaper();
            }
        }
        else
            g_free( file );

        ret = ( daemon_mode || ( desktop && desktop_or_deamon_initialized)  );
        goto out;
    }
#endif
    else /* open files/folders */
    {
        if( (daemon_mode || desktop) && ! desktop_or_deamon_initialized )
        {
            init_desktop_or_daemon();
        }
        else if ( files != default_files )
        {
            /* open files passed in command line arguments */
            ret = FALSE;
            for( file = files; *file; ++file )
            {
                char *real_path;
                tab_added = FALSE;

                if( ! **file )  /* skip empty string */
                    continue;

                real_path = dup_to_absolute_file_path( file );

                if( g_file_test( real_path, G_FILE_TEST_IS_DIR ) )
                {
                    // preload panel?
                    if ( !main_window )
                    {
                        if ( panel > 0 && panel < 5 )
                            // user specified panel
                            p = panel;
                        else
                        {
                            // use first visible panel
                            for ( p = 1; p < 5; p++ )
                            {
                                if ( xset_get_b_panel( p, "show" ) )
                                    break;
                            }
                        }
                        // set panel to load real_path on window creation
                        //     if panel has no saved tabs
                        if ( p < 5 )
                        {
                            set = xset_get_panel( p, "show" );
                            if ( !set->s )
                            {
                                set->s = g_strdup_printf( "///%s", real_path );
                                set->b = XSET_B_TRUE;
                                tab_added = TRUE;
                            }
                        }
                    }
                    // create main window if needed
                    if( G_UNLIKELY( ! main_window ) )
                    {
                        // initialize things required by folder view
                        if( G_UNLIKELY( ! daemon_mode ) )
                            init_folder();
                        main_window = create_main_window();
                    }
                    // add tab
					//startup_mode = TRUE;  //MOD needed for socket event add tab - not needed in v2
                    if ( !tab_added )
                    {
                        if ( panel > 0 && panel < 5 )
                        {
                            // change to user-specified panel
                            if ( !GTK_WIDGET_VISIBLE( main_window->panel[panel-1] ) )
                            {
                                // set panel to load real_path on panel show
                                //     if panel has no saved tabs
                                set = xset_get_panel( panel, "show" );
                                if ( !set->s )
                                {
                                    set->s = g_strdup_printf( "///%s", real_path );
                                    tab_added = TRUE;
                                }
                                // show panel
                                set->b = XSET_B_TRUE;
                                show_panels_all_windows( NULL, main_window );
                            }
                            main_window->curpanel = panel;
                            main_window->notebook = main_window->panel[panel-1];
                        }
                        if ( !tab_added )
                        {
                            if ( reuse_tab )
                            {
                                main_window_open_path_in_current_tab( main_window,
                                                                        real_path );
                                reuse_tab = FALSE;
                            }
                            else
                                fm_main_window_add_new_tab( main_window, real_path );
                        }
                    }
                    gtk_window_present( GTK_WINDOW( main_window ) );
                    ret = TRUE;
                }
                else
                    open_file( real_path );
                g_free( real_path );
            }
        }
        else
        {
            // no files specified, just create window with default tabs
            if( G_UNLIKELY( ! main_window ) )
            {
                // initialize things required by folder view
                if( G_UNLIKELY( ! daemon_mode ) )
                    init_folder();
                main_window = create_main_window();
            }
            else
                gtk_window_present( GTK_WINDOW( main_window ) );
            //startup_mode = TRUE;  //MOD needed for socket event add tab - not needed in v2      
        }
    }

out:
    if( files != default_files )
        g_strfreev( files );

    files = NULL;
    return ret;
}

void tmp_clean()
{
    char* cmd = g_strdup_printf( "rm -rf %s", xset_get_user_tmp_dir() );
    g_spawn_command_line_async( cmd, NULL );
    g_free( cmd );
}

int main ( int argc, char *argv[] )
{
    gboolean run = FALSE;
    GError* err = NULL;
    
#ifdef ENABLE_NLS
    bindtextdomain ( GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR );
    bind_textdomain_codeset ( GETTEXT_PACKAGE, "UTF-8" );
    textdomain ( GETTEXT_PACKAGE );
#endif

    /* Initialize multithreading
         No matter we use threads or not, it's safer to initialize this earlier. */
#ifdef _DEBUG_THREAD
    gdk_threads_set_lock_functions(_debug_gdk_threads_enter, _debug_gdk_threads_leave);
#endif
    g_thread_init( NULL );
    gdk_threads_init ();

    /* initialize GTK+ and parse the command line arguments */
    if( G_UNLIKELY( ! gtk_init_with_args( &argc, &argv, "", opt_entries, GETTEXT_PACKAGE, &err ) ) )
        return 1;

#if HAVE_HAL
    /* If the user wants to mount/umount/eject a device */
    if( G_UNLIKELY( mount || umount || eject ) )
        return handle_mount( argv );
#endif

    /* ensure that there is only one instance of spacefm.
         if there is an existing instance, command line arguments
         will be passed to the existing instance, and exit() will be called here.  */
    single_instance_check();

    /* initialize the file alteration monitor */
    if( G_UNLIKELY( ! vfs_file_monitor_init() ) )
    {
        ptk_show_error( NULL, _("Error"), _("Error: Unable to establish connection with FAM.\n\nDo you have \"FAM\" or \"Gamin\" installed and running?") );
        vfs_file_monitor_clean();
        //free_settings();
        return 1;
    }

    /* check if the filename encoding is UTF-8 */
    vfs_file_info_set_utf8_filename( g_get_filename_charsets( NULL ) );

    /* Initialize our mime-type system */
    vfs_mime_type_init();

    load_settings( config_dir );    /* load config file */  //MOD was before vfs_file_monitor_init

    app_settings.sdebug = sdebug;
    
/*
    // temporarily turn off desktop if needed
    if( G_LIKELY( no_desktop ) )
    {
        // No matter what the value of show_desktop is, we don't showdesktop icons
        // if --no-desktop argument is passed by the users.
        old_show_desktop = app_settings.show_desktop;
        // This config value will be restored before saving config files, if needed.
        app_settings.show_desktop = FALSE;
    }
*/
    /* If we reach this point, we are the first instance.
     * Subsequent processes will exit() inside single_instance_check and won't reach here.
     */

    /* handle the parsed result of command line args */
    run = handle_parsed_commandline_args();
 
    if( run )   /* run the main loop */
        gtk_main();

    single_instance_finalize();

    if( desktop && desktop_or_deamon_initialized )  // desktop was app_settings.show_desktop
        fm_turn_off_desktop_icons();

/*
    if( no_desktop )    // desktop icons is temporarily supressed
    {
        if( old_show_desktop )  // restore original settings
        {
            old_show_desktop = app_settings.show_desktop;
            app_settings.show_desktop = TRUE;
        }
    }
*/

/*
    if( run && xset_get_b( "main_save_exit" ) )
    {
        char* err_msg = save_settings();
        if ( err_msg )
            printf( "spacefm: Error: Unable to save session\n       %s\n", err_msg );
    }
*/
    vfs_volume_finalize();
    vfs_mime_type_clean();
    vfs_file_monitor_clean();
    tmp_clean();
    free_settings();

    return 0;
}

void open_file( const char* path )
{
    GError * err;
    char *msg, *error_msg;
    VFSFileInfo* file;
    VFSMimeType* mime_type;
    gboolean opened;
    char* app_name;

    if ( ! g_file_test( path, G_FILE_TEST_EXISTS ) )
    {
        ptk_show_error( NULL, _("Error"), _( "File doesn't exist" ) );
        return ;
    }

    file = vfs_file_info_new();
    vfs_file_info_get( file, path, NULL );
    mime_type = vfs_file_info_get_mime_type( file );
    opened = FALSE;
    err = NULL;

    app_name = vfs_mime_type_get_default_action( mime_type );
    if ( app_name )
    {
        opened = vfs_file_info_open_file( file, path, &err );
        g_free( app_name );
    }
    else
    {
        VFSAppDesktop* app;
        GList* files;

        app_name = (char *) ptk_choose_app_for_mime_type( NULL, mime_type );
        if ( app_name )
        {
            app = vfs_app_desktop_new( app_name );
            if ( ! vfs_app_desktop_get_exec( app ) )
                app->exec = g_strdup( app_name ); /* This is a command line */
            files = g_list_prepend( NULL, (gpointer) path );
            opened = vfs_app_desktop_open_files( gdk_screen_get_default(),
                                                 NULL, app, files, &err );
            g_free( files->data );
            g_list_free( files );
            vfs_app_desktop_unref( app );
            g_free( app_name );
        }
        else
            opened = TRUE;
    }

    if ( !opened )
    {
        char * disp_path;
        if ( err && err->message )
        {
            error_msg = err->message;
        }
        else
            error_msg = _( "Don't know how to open the file" );
        disp_path = g_filename_display_name( path );
        msg = g_strdup_printf( _( "Unable to open file:\n\"%s\"\n%s" ), disp_path, error_msg );
        g_free( disp_path );
        ptk_show_error( NULL, _("Error"), msg );
        g_free( msg );
        if ( err )
            g_error_free( err );
    }
    vfs_mime_type_unref( mime_type );
    vfs_file_info_unref( file );
}

/* After opening any window/dialog/tool, this should be called. */
void pcmanfm_ref()
{
    ++n_pcmanfm_ref;
}

/* After closing any window/dialog/tool, this should be called.
 * If the last window is closed and we are not a deamon, pcmanfm will quit.
 */
gboolean pcmanfm_unref()
{
    --n_pcmanfm_ref;
    if( 0 == n_pcmanfm_ref && ! daemon_mode && !desktop )
        gtk_main_quit();
    return FALSE;
}
