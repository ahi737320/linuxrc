/*
 *
 * install.c           Handling of installation
 *
 * Copyright (c) 1996-2001  Hubert Mantel, SuSE GmbH  (mantel@suse.de)
 *
 */

#include "dietlibc.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <string.h>
#include <signal.h>
#include <netdb.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/swap.h>
#include <sys/socket.h>
#include <sys/reboot.h>
#include <arpa/inet.h>

#include <hd.h>

#include "global.h"
#include "linuxrc.h"
#include "text.h"
#include "util.h"
#include "dialog.h"
#include "window.h"
#include "net.h"
#include "display.h"
#include "rootimage.h"
#include "module.h"
#include "keyboard.h"
#include "file.h"
#include "info.h"
#include "ftp.h"
#include "install.h"
#include "settings.h"
#include "auto2.h"


#define YAST2_COMMAND   "/usr/lib/YaST2/bin/YaST2.start"
#define YAST1_COMMAND   "/sbin/YaST"
#define SETUP_COMMAND   "/sbin/inst_setup"

static char  inst_rootimage_tm [MAX_FILENAME];
static int   inst_rescue_im = FALSE;
static int   inst_loopmount_im = FALSE;
static char *inst_tmpmount_tm = "/tmp/loopmount";
static char  inst_rescuefile_tm [MAX_FILENAME];
static char *inst_demo_sys_tm = "/suse/images/cd-demo";

static int   inst_mount_harddisk      (void);
static int   inst_try_cdrom           (char *device_tv);
static int   inst_mount_cdrom         (int show_err);
static int   inst_mount_nfs           (void);
static int   inst_start_install       (void);
static int   inst_start_rescue        (void);
//static void  inst_start_shell         (char *tty_tv);
static int   inst_prepare             (void);
static int   inst_execute_yast        (void);
static int   inst_check_floppy        (void);
static int   inst_commit_install      (void);
static int   inst_choose_source       (void);
static int   inst_choose_source_cb    (dia_item_t di);
static int   inst_menu_cb             (dia_item_t di);
static int   inst_init_cache          (void);
static int   inst_umount              (void);
static int   inst_mount_smb           (void);
static int   inst_ftp                 (void);
static int   inst_get_ftpsetup        (void);
static int   inst_choose_yast_version (void);
static int   inst_update_cd           (void);
static void  inst_swapoff             (void);

static int inst_get_smbserver(void);
static int inst_get_smbsetup (void);

#ifdef OBSOLETE_YAST_LIVECD
/* 'Live' entry in yast.inf */
static int yast_live_cd = 0;
#endif

static dia_item_t di_inst_menu_last = di_none;
static dia_item_t di_inst_choose_source_last = di_none;

int inst_auto_install (void)
    {
    int       rc_ii;


    if (!auto_ig)
        return (-1);

    inst_rescue_im = FALSE;

    switch (bootmode_ig)
        {
        case BOOTMODE_CD:
        case BOOTMODE_CDWITHNET:
            rc_ii = inst_mount_cdrom (1);
            break;
        case BOOTMODE_SMB:
            rc_ii = inst_mount_smb ();
            break;
        case BOOTMODE_NET:
            rc_ii = inst_mount_nfs ();
            break;
        case BOOTMODE_HARDDISK:
            rc_ii = inst_mount_harddisk();
            break;
        default:
            rc_ii = -1;
            break;
        }

    if (!rc_ii)
        rc_ii = inst_check_instsys ();

    if (rc_ii)
        {
        inst_umount ();
        return (-1);
        }

    if (ramdisk_ig)
        {
        rc_ii = root_load_rootimage (inst_rootimage_tm);
        inst_umount ();
        if (rc_ii)
            return (rc_ii);

        mkdir (inst_mountpoint_tg, 0777);
        rc_ii = util_try_mount (RAMDISK_2, inst_mountpoint_tg,
                                MS_MGC_VAL | MS_RDONLY, 0);
        if (rc_ii)
            return (rc_ii);
        }

    return (inst_execute_yast ());
    }


int inst_start_demo (void)
    {
    int    rc_ii;
    char   filename_ti [MAX_FILENAME];
    FILE  *file_pri;
    char   line_ti [MAX_X];
    int    test_ii = FALSE;

    if (!auto2_ig)
        {
        if (demo_ig)
            if (!info_eide_cd_exists ())
                {
                rc_ii = mod_auto (MOD_TYPE_SCSI);
                if (rc_ii || !info_scsi_cd_exists ())
                    (void) mod_auto (MOD_TYPE_OTHER);
                }

        if (strcmp (rootimage_tg, "test"))
            test_ii = FALSE;
        else
            test_ii = TRUE;

        if (test_ii)
            rc_ii = inst_mount_nfs ();
        else
            {
            if (!demo_ig)
                (void) dia_message (txt_get (TXT_INSERT_LIVECD), MSGTYPE_INFO);

            rc_ii = inst_mount_cdrom (1);
            }

        if (rc_ii)
            return (rc_ii);
        }
    else
        {
        if ((action_ig & ACT_DEMO_LANG_SEL))
            {
            util_manual_mode();
            util_disp_init ();
            set_choose_language ();
            util_print_banner ();
            set_choose_keytable (1);
            }
        }

    sprintf (filename_ti, "%s/%s", mountpoint_tg, inst_demo_sys_tm);
    if (!util_check_exist (filename_ti))
        {
        util_disp_init();
        dia_message (txt_get (TXT_RI_NOT_FOUND), MSGTYPE_ERROR);
        inst_umount ();
        return (-1);
        }

    rc_ii = root_load_rootimage (filename_ti);
    inst_umount ();

    if (rc_ii)
        return (rc_ii);

    if (util_try_mount (RAMDISK_2, mountpoint_tg, 0, 0))
        return (-1);

    file_write_install_inf (mountpoint_tg);

    sprintf (filename_ti, "%s/%s", mountpoint_tg, "etc/fstab");
    file_pri = fopen (filename_ti, "a");
    // TODO:SMB???
    if (bootmode_ig == BOOTMODE_NET && !*livesrc_tg)
        {
        sprintf(line_ti,
          "%s:%s /S.u.S.E. nfs ro,nolock 0 0\n",
          inet_ntoa(config.net.server.ip),
          config.serverdir ?: ""
        );
        }
    else
        {
        sprintf(line_ti,
          "/dev/%s /S.u.S.E. %s ro 0 0\n",
          *livesrc_tg ? livesrc_tg : cdrom_tg,
          *livesrc_tg ? "auto" : "iso9660"
        );
        }

    fprintf (file_pri, line_ti);
    fclose (file_pri);
    inst_umount ();
    return (0);
    }


int inst_menu()
{
  dia_item_t di;
  dia_item_t items[] = {
    di_inst_install,
    di_inst_demo,
    di_inst_system,
    di_inst_rescue,
    di_inst_eject,
    di_inst_update,
    di_none
  };

  items[(action_ig & ACT_DEMO) ? 0 : 1] = di_skip;
  if(!yast2_update_ig) items[5] = di_skip;

  di = dia_menu2(txt_get(TXT_MENU_START), 40, inst_menu_cb, items, di_inst_menu_last);

  return di == di_none ? 1 : 0;
}


/*
 * return values:
 * -1    : abort (aka ESC)
 *  0    : ok
 *  other: stay in menu
 */
int inst_menu_cb(dia_item_t di)
{
  int error = 0;
  char s[64];
  int rc = 1;

  di_inst_menu_last = di;

  switch(di) {
    case di_inst_install:
      error = inst_start_install();
     /*
      * Fall through to the main menu if we return from a failed installation
      * attempt.
      */
      if(config.redraw_menu) rc = -1;
      break;

    case di_inst_demo:
      error = inst_start_demo();
      if(config.redraw_menu) rc = -1;
      break;

    case di_inst_system:
      error = root_boot_system();
      break;

    case di_inst_rescue:
      error = inst_start_rescue();
      break;

    case di_inst_eject:
      sprintf(s, "/dev/%s", cdrom_tg);
      util_eject_cdrom(*cdrom_tg ? s : NULL);
      error = 1;
      break;

    case di_inst_update:
      inst_update_cd();
      error = 1;
      break;

    default:
  }

  config.redraw_menu = 0;

  if(!error) rc = 0;

  return rc;
}


int inst_choose_source()
{
  dia_item_t di;
  dia_item_t items[] = {
    di_source_cdrom,
    di_source_nfs,
    di_source_ftp,
    di_source_smb,
    di_source_hd,
    di_source_floppy,
    di_none
  };

  inst_umount();

  config.net.smb_available = config.test || util_check_exist("/bin/smbmount");

  if(!config.net.smb_available) items[3] = di_skip;
  if(!inst_rescue_im) items[5] = di_skip;

  di = dia_menu2(txt_get(TXT_CHOOSE_SOURCE), 33, inst_choose_source_cb, items, di_inst_choose_source_last);

  return di == di_none ? -1 : 0;
}


/*
 * return values:
 * -1    : abort (aka ESC)
 *  0    : ok
 *  other: stay in menu
 */
int inst_choose_source_cb(dia_item_t di)
{
  int error = FALSE;
  char tmp[200];

  di_inst_choose_source_last = di;

  switch(di) {
    case di_source_cdrom:
      error = inst_mount_cdrom(0);
      if(error) {
        sprintf(tmp, txt_get(TXT_INSERT_CD), 1);
        dia_message(tmp, MSGTYPE_INFO);
        error = inst_mount_cdrom(1);
      }
      break;

    case di_source_nfs:
      error = inst_mount_nfs();
      break;

    case di_source_ftp:
      error = inst_ftp();
      break;

    case di_source_smb:
      error = inst_mount_smb();
      break;

    case di_source_hd:
      error = inst_mount_harddisk();
      break;

    case di_source_floppy:
      error = inst_check_floppy();
      break;

    default:
  }

  if(!error && di != di_source_cdrom) {
    error = inst_check_instsys();
    if(error) dia_message(txt_get(TXT_RI_NOT_FOUND), MSGTYPE_ERROR);
  }

  if(error) inst_umount();

  return error ? 1 : 0;
}


static int inst_try_cdrom (char *device_tv)
    {
    char  path_device_ti [20];
    int   rc_ii;


    sprintf (path_device_ti, "/dev/%s", device_tv);
    rc_ii = mount (path_device_ti, mountpoint_tg, "iso9660", MS_MGC_VAL | MS_RDONLY, 0);
    return (rc_ii);
    }


static int inst_mount_cdrom (int show_err)
    {
    static char  *device_tab_ats [] =
                          {
                          "hdb",   "hdc",    "hdd",     "sr0",    "sr1",
                          "sr2",   "sr3",    "sr4",     "sr5",    "sr6",
                          "sr7",   "sr8",    "sr9",     "sr10",   "sr11",
                          "sr12",  "sr13",   "sr14",    "sr15",
                          "hda",   "hde",    "hdf",     "hdg",    "hdh",
                          "aztcd", "cdu535", "cm206cd", "gscd",   "sjcd",
                          "mcd",   "mcdx0",  "mcdx1",   "optcd",  "sonycd",
                          "sbpcd", "sbpcd1", "sbpcd2",  "sbpcd3", "pcd0",
                          "pcd1",  "pcd2",   "pcd3",
                          0
                          };
    int           rc_ii;
    int           i_ii = 0;
    char         *device_pci;
    window_t      win_ri;
    int           mount_success_ii = FALSE;

    if( bootmode_ig == BOOTMODE_CDWITHNET ) {
        rc_ii = net_config ();
    }
    else {
      bootmode_ig = BOOTMODE_CD;
    }

    dia_info (&win_ri, txt_get (TXT_TRY_CD_MOUNT));

    if (cdrom_tg [0])
        device_pci = cdrom_tg;
    else
        device_pci = device_tab_ats [i_ii++];

    rc_ii = inst_try_cdrom (device_pci);
    if (!rc_ii)
        {
        cdrom_drives++;
        mount_success_ii = TRUE;
        rc_ii = inst_check_instsys ();
        if (rc_ii)
            inst_umount ();
        }

    while (rc_ii < 0 && device_tab_ats [i_ii])
        {
        device_pci = device_tab_ats [i_ii++];
        rc_ii = inst_try_cdrom (device_pci);
        if (!rc_ii)
            {
            cdrom_drives++;
            mount_success_ii = TRUE;
            rc_ii = inst_check_instsys ();
            if (rc_ii)
                inst_umount ();
            }
        }

    win_close (&win_ri);

    if (rc_ii < 0)
        {
        if (show_err)
             dia_message (txt_get (mount_success_ii ? TXT_RI_NOT_FOUND :
                                                     TXT_ERROR_CD_MOUNT),
                         MSGTYPE_ERROR);
        }
    else
        strcpy (cdrom_tg, device_pci);

    return (rc_ii);
    }


int inst_mount_nfs()
{
  int rc;
  window_t win;
  char text[256 + MAX_FILENAME];

  bootmode_ig = BOOTMODE_NET;

  if((rc = net_config())) return rc;

  if(config.win && !auto_ig) {
    if((rc = net_get_address(txt_get(TXT_INPUT_SERVER), &config.net.server, 1))) return rc;
    if((rc = dia_input2(txt_get(TXT_INPUT_DIR), &config.serverdir, 30, 0))) return rc;
  }
  util_truncate_dir(config.serverdir);

  sprintf(text, txt_get(TXT_TRY_NFS_MOUNT), config.net.server.name, config.serverdir);
  dia_info(&win, text);

  system("portmap");

  rc = net_mount_nfs(inet_ntoa(config.net.server.ip), config.serverdir);
  win_close(&win);

  return rc;
}


static int inst_mount_harddisk (void)
    {
    int   rc_ii = 0;
    int   i_ii;
    char *mountpoint_pci;


    bootmode_ig = BOOTMODE_HARDDISK;
    do
        {
        if (!auto_ig)
            {
            rc_ii = dia_input (txt_get (TXT_ENTER_PARTITION), harddisk_tg, 17, 17);
            if (rc_ii)
                return (rc_ii);
            }

        if (!inst_rescue_im && !force_ri_ig)
            {
            mkdir (inst_tmpmount_tm, 0777);
            inst_loopmount_im = TRUE;
            mountpoint_pci = inst_tmpmount_tm;
            }
        else
            {
            inst_loopmount_im = FALSE;
            mountpoint_pci = mountpoint_tg;
            }

        i_ii = 0;
        do
            rc_ii = mount (harddisk_tg, mountpoint_pci, fs_types_atg [i_ii++],
                           MS_MGC_VAL | MS_RDONLY, 0);
        while (rc_ii && fs_types_atg [i_ii]);

        if (rc_ii)
            dia_message (txt_get (TXT_ERROR_HD_MOUNT), MSGTYPE_ERROR);
        else
            {
            fstype_tg = fs_types_atg [i_ii - 1];
            if (!auto_ig)
                {
                rc_ii = dia_input2 (txt_get (TXT_ENTER_HD_DIR), &config.serverdir, 30, 0);
                if (rc_ii)
                    {
                    inst_umount ();
                    return (rc_ii);
                    }

                util_truncate_dir (config.serverdir);
                }
            }
        }
    while (rc_ii);

    return (0);
    }


int inst_check_instsys (void)
    {
    char  filename_ti [MAX_FILENAME];
    char  filename2_ti [MAX_FILENAME];
    char *instsys_loop_ti = "/suse/setup/inst-img";
    int hdmount_ok = 0;

    if ((action_ig & ACT_RESCUE)) 
        {
        action_ig &= ~ACT_RESCUE;
        inst_rescue_im = TRUE;
        }

    if (memory_ig > 8000000)
        strcpy (inst_rescuefile_tm, "/suse/images/rescue");
    else
        strcpy (inst_rescuefile_tm, "/disks/rescue");

    switch (bootmode_ig)
        {
        case BOOTMODE_FLOPPY:
            ramdisk_ig = TRUE;
            strcpy (inst_rootimage_tm, config.floppies ? config.floppy_dev[config.floppy] : "/dev/fd0");
            break;
        case BOOTMODE_HARDDISK:
          if(inst_loopmount_im) {
            ramdisk_ig = FALSE;
            sprintf(filename_ti, "%s%s", inst_tmpmount_tm, config.serverdir ?: "");
            if(!mount(filename_ti, mountpoint_tg, "none", MS_BIND, 0)) {
              sprintf(filename_ti, "%s%s", mountpoint_tg, installdir_tg);
              sprintf(filename2_ti, "%s%s", mountpoint_tg, instsys_loop_ti);
              if(!util_mount_loop(filename2_ti, filename_ti)) {
                hdmount_ok = 1;
              }
              else if(!inst_rescue_im && util_check_exist(filename_ti)) {
                hdmount_ok = 1;
              }
            }
          }
          if(!hdmount_ok) {
            ramdisk_ig = TRUE;
            sprintf(
              inst_rootimage_tm, "%s%s%s",
              mountpoint_tg,
              config.serverdir ?: "",
              inst_rescue_im == TRUE ? inst_rescuefile_tm : rootimage_tg
            );
          }
          break;
        case BOOTMODE_CDWITHNET:
        case BOOTMODE_CD:
        case BOOTMODE_NET:
	case BOOTMODE_SMB:
            ramdisk_ig = FALSE;
            sprintf (filename_ti, "%s%s", mountpoint_tg, installdir_tg);
            if (inst_rescue_im || force_ri_ig || !util_check_exist (filename_ti))
                ramdisk_ig = TRUE;
            sprintf (inst_rootimage_tm, "%s%s", mountpoint_tg,
                     inst_rescue_im == TRUE ? inst_rescuefile_tm : rootimage_tg);
            if (!util_check_exist (inst_rootimage_tm))
                {
                if (util_check_exist (filename_ti))
                    ramdisk_ig = FALSE;
                else
                    sprintf (inst_rootimage_tm, "%s/%s", mountpoint_tg, inst_demo_sys_tm);
                }
            break;
        case BOOTMODE_FTP:
            ramdisk_ig = TRUE;
            sprintf (inst_rootimage_tm, "%s%s", config.serverdir ?: "",
                     inst_rescue_im == TRUE ? inst_rescuefile_tm : rootimage_tg);
            break;
        default:
            break;
        }

    if (bootmode_ig != BOOTMODE_FTP && ramdisk_ig &&
        !util_check_exist (inst_rootimage_tm))
        return (-1);
    else
        return (0);
    }


static int inst_start_install (void)
    {
    int       rc_ii;

    inst_rescue_im = FALSE;
    rc_ii = inst_choose_source ();
    if (rc_ii)
        return (rc_ii);

    if (ramdisk_ig)
        {
        rc_ii = root_load_rootimage (inst_rootimage_tm);
        fprintf (stderr, "Loading of rootimage returns %d\n", rc_ii);
        inst_umount ();
        if (rc_ii)
            return (rc_ii);

        mkdir (inst_mountpoint_tg, 0777);
        rc_ii = util_try_mount (RAMDISK_2, inst_mountpoint_tg,
                                MS_MGC_VAL | MS_RDONLY, 0);
        fprintf (stderr, "Mounting of inst-sys returns %d\n", rc_ii);
        if (rc_ii)
            return (rc_ii);
        }

    rc_ii = inst_execute_yast ();

    return (rc_ii);
    }


static int inst_start_rescue (void)
    {
    int   rc_ii;


    inst_rescue_im = TRUE;
    rc_ii = inst_choose_source ();
    if (rc_ii)
        return (rc_ii);

    rc_ii = root_load_rootimage (inst_rootimage_tm);
    inst_umount ();
    return (rc_ii);
    }


#if 0
static void inst_start_shell (char *tty_tv)
    {
    char  *args_apci [] = { "bash", 0 };
    char  *env_pci [] =   { "TERM=linux",
                            "PS1=`pwd -P` # ",
                            "HOME=/",
                            "PATH=/lbin:/bin:/sbin:/usr/bin:/usr/sbin:/usr/lib/YaST2/bin", 0 };
    int    fd_ii;


    if (!fork ())
        {
        fclose (stdin);
        fclose (stdout);
        fclose (stderr);
        setsid ();
        fd_ii = open (tty_tv, O_RDWR);
        ioctl (fd_ii, TIOCSCTTY, (void *)1);
        dup (fd_ii);
        dup (fd_ii);

        execve ("/bin/bash", args_apci, env_pci);
        fprintf (stderr, "Couldn't start shell (errno = %d)\n", errno);
        exit (-1);
        }
    }
#endif


/*
 * Do some basic preparations before we can run programs from the
 * installation system. More is done later in SETUP_COMMAND.
 *
 * Note: the instsys must already be mounted at this point.
 *
 */
int inst_prepare()
{
  char *links[] = { "/bin", "/lib", "/sbin", "/usr" };
  char link_source[MAX_FILENAME];
  char instsys[MAX_FILENAME];
  int i = 0;
  int rc = 0;

  mod_free_modules();
  if(!config.initrd_has_ldso)
    rename("/bin", "/.bin");

  if(inst_loopmount_im) {
    sprintf(instsys, "%s%s", mountpoint_tg, installdir_tg);
  }
  else {
    if(ramdisk_ig)
      strcpy(instsys, inst_mountpoint_tg);
    else
      sprintf(instsys, "%s%s", mountpoint_tg, installdir_tg);
  }

  if(config.instsys) free(config.instsys);
  config.instsys = strdup(instsys);

  setenv("INSTSYS", instsys, TRUE);

  if(!config.initrd_has_ldso)
    for(i = 0; i < sizeof links / sizeof *links; i++) {
      if(!util_check_exist(links[i])) {
	unlink(links[i]);
	sprintf(link_source, "%s%s", instsys, links[i]);
	symlink(link_source, links[i]);
      }
    }

  setenv("PATH", "/lbin:/bin:/sbin:/usr/bin:/usr/sbin:/usr/lib/YaST2/bin", TRUE);

  if(serial_ig) {
    setenv("TERM", "vt100", TRUE);
    setenv("ESCDELAY", "1100", TRUE);
  }
  else {
    setenv("TERM", "linux", TRUE);
    setenv("ESCDELAY", "10", TRUE);
  }

  setenv("YAST_DEBUG", "/debug/yast.debug", TRUE);

  file_write_install_inf("");
  file_write_mtab();

  if(!ramdisk_ig) rc = inst_init_cache();

  return rc;
}


int inst_execute_yast()
{
  int rc_ii;
//  int i_ii = 0;
  int i, count;
//  window_t status_ri;
  char command_ti[256];

  rc_ii = inst_prepare();
  if(rc_ii) return rc_ii;

  if(inst_choose_yast_version()) {
    lxrc_killall(0);
    inst_umount ();
    if(ramdisk_ig) util_free_ramdisk("/dev/ram2");

    if(!config.initrd_has_ldso) {
      unlink("/bin");
      rename("/.bin", "/bin");
    }
    return -1;
  }

#if 0
  if(!auto2_ig) dia_status_on(&status_ri, txt_get(TXT_START_YAST));
#endif

  lxrc_set_modprobe("/sbin/modprobe");

  if(util_check_exist("/sbin/update")) system("/sbin/update");

#if 0
  if(!auto2_ig) {
    while(i_ii < 50) {
      dia_status(&status_ri, i_ii++);
      usleep(10000);
    }
  }
#endif

  util_start_shell("/dev/tty2", "/bin/bash", 1);

  if(memory_ig < MEM_LIMIT_SWAP_MSG) {
    if(config.win) dia_message(txt_get(TXT_LITTLE_MEM), MSGTYPE_ERROR);
  }
  else {
    util_start_shell("/dev/tty5", "/bin/bash", 1);
    util_start_shell("/dev/tty6", "/bin/bash", 1);
  }

#if 0
  if(!auto2_ig) {
    while(i_ii <= 100) {
      dia_status (&status_ri, i_ii++);
      usleep(10000);
    }
    win_close(&status_ri);
  }
#endif

  disp_set_color(COL_WHITE, COL_BLACK);
  if(config.win) util_disp_done();

  sprintf(command_ti, "%s%s", config.instsys, SETUP_COMMAND);

//  deb_str(command_ti);

  if(util_check_exist(command_ti)) {
    sprintf(command_ti, "%s%s yast%d%s",
      config.instsys,
      SETUP_COMMAND,
      yast_version_ig == 2 ? 2 : 1,
      (action_ig & ACT_YAST2_AUTO_INSTALL) ? " --autofloppy" : ""
    );
    fprintf(stderr, "starting yast%d\n", yast_version_ig == 2 ? 2 : 1);
  }
  else {
    sprintf(command_ti, "%s%s",
      yast_version_ig == 2 ? YAST2_COMMAND : YAST1_COMMAND,
      auto_ig ? " --autofloppy" : ""
    );
    fprintf(stderr, "starting \"%s\"\n", command_ti);
  }

  deb_str(command_ti);
  rc_ii = system(command_ti);

  fprintf(stderr,
    "yast%d return code is %d (errno = %d)\n",
    yast_version_ig == 2 ? 2 : 1,
    rc_ii,
    rc_ii ? errno : 0
  );

#ifdef LXRC_DEBUG
  if((guru_ig & 1)) { printf("a shell for you...\n"); system("/bin/sh"); }
#endif

  lxrc_set_modprobe("/etc/nothing");

  /* Redraw erverything and go back to the main menu. */
  config.redraw_menu = 1;

  fprintf(stderr, "sync...");
  sync();
  fprintf(stderr, " ok\n");

  i = file_read_yast_inf();
  if(!rc_ii) rc_ii = i;

  // if(!auto2_ig) disp_restore_screen();
  disp_cursor_off();
  kbd_reset();

  yast_version_ig = 0;

  if(rc_ii || !auto2_ig) {
    util_manual_mode();
    util_disp_init();
  }

  if(rc_ii) {
    dia_message(txt_get(TXT_ERROR_INSTALL), MSGTYPE_ERROR);
  }

  lxrc_killall(0);

#if 0
  system("rm -f /tmp/stp* > /dev/null 2>&1");
  system("rm -f /var/lib/YaST/* > /dev/null 2>&1");
#endif

#if 0
  system("umount -a -tnoproc,nousbdevfs,nominix > /dev/null 2>&1");
#endif

  /* if the initrd has an ext2 fs, we've just made / read-only */
  mount(0, "/", 0, MS_MGC_VAL | MS_REMOUNT, 0);

//  inst_umount();
  if(ramdisk_ig) util_free_ramdisk("/dev/ram2");

  if(!config.initrd_has_ldso) {
    unlink("/bin");
    rename("/.bin", "/bin");
  }
  /* turn off swap */
  inst_swapoff();

#ifdef LXRC_DEBUG
  if((guru_ig & 2)) {
    util_manual_mode();
    util_disp_init();
    dia_message("Installation part 0 finished...", MSGTYPE_INFO);
  }
#endif

  /* wait a bit */
  count = 5;
  while((i = inst_umount()) == EBUSY && count--) sleep(1);

#ifdef LXRC_DEBUG
  if((guru_ig & 2)) {
    util_manual_mode();
    util_disp_init();
    dia_message("Installation part 1 finished...", MSGTYPE_INFO);
  }
#endif

  if(!rc_ii) {
    rc_ii = inst_commit_install();
    if(rc_ii) {
      util_manual_mode();
      util_disp_init();
    }
  }

  return rc_ii;
}


/*
 * Look for a usable (aka with medium) floppy drive.
 *
 * return: 0: ok, < 0: failed
 */
// ####### We should make sure the floppy has the rescue system on it!
int inst_check_floppy()
{
  int i, fd = -1;
  char *s;

  bootmode_ig = BOOTMODE_FLOPPY;
  i = dia_message(txt_get(TXT_INSERT_DISK), MSGTYPE_INFO);
  if(i) return i;

  for(i = -1; i < config.floppies; i++) {
    /* try last working floppy first */
    if(i == config.floppy) continue;
    s = config.floppy_dev[i == -1 ? config.floppy : i];
    if(!s) continue;
    fd = open(s, O_RDONLY);
    if(fd < 0) continue;
    config.floppy = i == -1 ? config.floppy : i;
    break;
  }

  if(fd < 0)
    dia_message(txt_get(TXT_ERROR_READ_DISK), MSGTYPE_ERROR);
  else
    close(fd);

  return fd < 0 ? fd : 0;
}


int inst_commit_install()
{
  int err = 0;
  window_t win;

  if(reboot_ig) {

    if(config.rebootmsg) {
      disp_clear_screen();
      util_disp_init();
      dia_message(txt_get(TXT_DO_REBOOT), MSGTYPE_INFO);
    }

    reboot(RB_AUTOBOOT);
    err = -1;
  }
  else {
#ifdef OBSOLETE_YAST_LIVECD
    if(yast_live_cd) {
      util_disp_init();
      dia_message(txt_get(TXT_INSERT_LIVECD), MSGTYPE_INFO);
    }
    else
#endif
    {
      if(auto_ig) {
        util_disp_init();
        dia_info(&win, txt_get(TXT_INSTALL_SUCCESS));
        sleep(2);
        win_close(&win);
      }
      else {

#if 0 /* ifndef __PPC__ */
        if(!auto2_ig) {
          util_disp_init();
          dia_message(txt_get(TXT_INSTALL_SUCCESS), MSGTYPE_INFO);
        }
#endif

      }
    }
  }

  return err;
}


static int inst_init_cache (void)
    {
    char     *files_ati [] = {
                             "/lib/libc.so.6",
                             "/lib/libc.so.5",
                             "/lib/ld.so",
                             "/bin/bash",
                             "/sbin/YaST"
                             };
    int32_t   size_li;
    long      allsize_li;
    int       dummy_ii;
    char      buffer_ti [10240];
    window_t  status_ri;
    int       i_ii;
    int       fd_ii;
    int       percent_ii;
    int       old_percent_ii;
    int       read_ii;


    if (memory_ig < MEM_LIMIT_CACHE_LIBS)
        return (0);

    dia_status_on (&status_ri, txt_get (TXT_PREPARE_INST));
    allsize_li = 0;
    for (i_ii = 0; i_ii < sizeof (files_ati) / sizeof (files_ati [0]); i_ii++)
        if (!util_fileinfo (files_ati [i_ii], &size_li, &dummy_ii))
            allsize_li += size_li;

    if (allsize_li)
        {
        size_li = 0;
        old_percent_ii = 0;

        for (i_ii = 0; i_ii < sizeof (files_ati) / sizeof (files_ati [0]); i_ii++)
            {
            fd_ii = open (files_ati [i_ii], O_RDONLY);
            while ((read_ii = read (fd_ii, buffer_ti, sizeof (buffer_ti))) > 0)
                {
                size_li += (long) read_ii;
                percent_ii = (int) ((size_li * 100) / allsize_li);
                if (percent_ii != old_percent_ii)
                    {
                    dia_status (&status_ri, percent_ii);
                    old_percent_ii = percent_ii;
                    }
                }
            close (fd_ii);
            }
        }
    win_close (&status_ri);
    return (0);
    }


int inst_umount()
{
  int i = 0, j;
  char fname[MAX_FILENAME];

  if(inst_loopmount_im) {
    sprintf(fname, "%s%s", mountpoint_tg, installdir_tg);
    util_umount_loop(fname);
    util_umount(mountpoint_tg);
    j = util_umount(inst_tmpmount_tm);
    if(j == EBUSY) i = EBUSY;
    rmdir(inst_tmpmount_tm);
    inst_loopmount_im = FALSE;
  }
  else {
    j = util_umount(mountpoint_tg);
    if(j == EBUSY) i = EBUSY;
  }

  j = util_umount(inst_mountpoint_tg);
  if(j == EBUSY) i = EBUSY;

  return i;
}


int inst_get_smbserver()
{
  int rc;

  if(net_get_address(txt_get(TXT_SMB_ENTER_SERVER), &config.net.server, 1)) return -1;
  if((rc = dia_input2(txt_get(TXT_SMB_ENTER_SHARE), &config.serverdir, 20, 0))) return rc;

  return 0;
}


int inst_get_smbsetup()
{
  int rc;

  rc = dia_yesno(txt_get(TXT_SMB_GUEST_LOGIN), YES);

  if(rc == ESCAPE) return -1;

  if(rc == YES) {
    if(config.net.user) free(config.net.user);
    config.net.user = NULL;
    if(config.net.password) free(config.net.password);
    config.net.password = NULL;
  }
  else {
    if((rc = dia_input2(txt_get(TXT_SMB_ENTER_USER), &config.net.user, 20, 0))) return rc;
    if((rc = dia_input2(txt_get(TXT_SMB_ENTER_PASSWORD), &config.net.password, 20, 1))) return rc;
    if((rc = dia_input2(txt_get(TXT_SMB_ENTER_WORKGROUP), &config.net.workgroup, 20, 0))) return rc;
  }

  return 0;
}


int inst_ftp()
{
  int rc;
  window_t win;
  char buf[256];

  if(!inst_rescue_im && memory_ig <= MEM_LIMIT_RAMDISK_FTP) {
    sprintf(buf, txt_get(TXT_NOMEM_FTP), (MEM_LIMIT_RAMDISK_FTP >> 20) + 2);
    dia_message(buf, MSGTYPE_ERROR);
    return -1;
  }

  if((rc = net_config())) return rc;

  do {
    if((rc = net_get_address(txt_get(TXT_INPUT_FTPSERVER), &config.net.server, 1))) return rc;
    if((rc = inst_get_ftpsetup())) return rc;

    dia_info(&win, txt_get(TXT_TRY_REACH_FTP));
    rc = util_open_ftp(inet_ntoa(config.net.server.ip));
    win_close(&win);

    if(rc < 0) {
      util_print_ftp_error(rc);
    }
    else {
      ftpClose(rc);
      rc = 0;
    }
  }
  while(rc);

  if(inst_rescue_im) {
    if(config.serverdir) free(config.serverdir);
    config.serverdir = strdup("/pub/SuSE-Linux/current");
  }

  if(dia_input2(txt_get(TXT_INPUT_DIR), &config.serverdir, 30, 0)) return -1;
  util_truncate_dir(config.serverdir);

  bootmode_ig = BOOTMODE_FTP;

  return 0;
}


static int inst_get_ftpsetup (void)
    {
    int             rc_ii;
    char            tmp_ti [MAX_FILENAME];
    struct in_addr  dummy_ri;
    int             i_ii;


    rc_ii = dia_yesno (txt_get (TXT_ANONYM_FTP), NO);
    if (rc_ii == ESCAPE)
        return (-1);

    if (rc_ii == NO)
        {
        strcpy (ftp_user_tg, "anonymous");
        strcpy (ftp_password_tg, "root@");
        }
    else
        {
        strcpy (tmp_ti, ftp_user_tg);
        rc_ii = dia_input (txt_get (TXT_ENTER_FTPUSER), tmp_ti,
                           sizeof (ftp_user_tg) - 1, 20);
        if (rc_ii)
            return (rc_ii);
        strcpy (ftp_user_tg, tmp_ti);

        strcpy (tmp_ti, ftp_password_tg);
        passwd_mode_ig = TRUE;
        rc_ii = dia_input (txt_get (TXT_ENTER_FTPPASSWD), tmp_ti,
                           sizeof (ftp_password_tg) - 1, 20);
        passwd_mode_ig = FALSE;
        if (rc_ii)
            return (rc_ii);
        strcpy (ftp_password_tg, tmp_ti);
        }


    rc_ii = dia_yesno (txt_get (TXT_WANT_FTPPROXY), NO);
    if (rc_ii == ESCAPE)
        return (-1);

    if (rc_ii == YES)
        {
        strcpy (tmp_ti, ftp_proxy_tg);
        do
            {
            rc_ii = dia_input (txt_get (TXT_ENTER_FTPPROXY), tmp_ti,
                               sizeof (ftp_proxy_tg) - 1, 30);
            if (rc_ii)
                return (rc_ii);

            if (isdigit (tmp_ti [0]))
                {
                rc_ii = net_check_address (tmp_ti, &dummy_ri);
                if (rc_ii)
                    (void) dia_message (txt_get (TXT_INVALID_INPUT),
                                        MSGTYPE_ERROR);
                }
            }
        while (rc_ii);
        strcpy (ftp_proxy_tg, tmp_ti);

        if (ftp_proxyport_ig == -1)
            tmp_ti [0] = 0;
        else
            sprintf (tmp_ti, "%d", ftp_proxyport_ig);

        do
            {
            rc_ii = dia_input (txt_get (TXT_ENTER_FTPPORT), tmp_ti,
                               6, 6);
            if (rc_ii)
                return (rc_ii);

            for (i_ii = 0; i_ii < strlen (tmp_ti); i_ii++)
                if (!isdigit (tmp_ti [i_ii]))
                    rc_ii = -1;

            if (rc_ii)
                (void) dia_message (txt_get (TXT_INVALID_INPUT), MSGTYPE_ERROR);
            }
        while (rc_ii);

        ftp_proxyport_ig = atoi (tmp_ti);
        }
    else
        {
        ftp_proxy_tg [0] = 0;
        ftp_proxyport_ig = -1;
        }

    return (0);
    }


int inst_mount_smb()
{
  int rc;
  window_t  win_ri;
  char msg[256];

  rc = net_config();
  if(rc) return rc;

    /*******************************************************************
  
    CD location
  
  	 server              :  SERVER
  	 server IP (optional):  SERVER_IP
  	 share               :  SHARE
  
    authentification for this CD-ROM share
  
  	 [X] Guest login  or
  
  	 username            :  USERNAME
  	 password            :  PASSWORD
  	 workgroup (optional):  WORKGROUP
  
    *******************************************************************/
    
  rc = inst_get_smbserver();
  if(rc) return rc;

  rc = inst_get_smbsetup();
  if(rc) return rc;

  sprintf(msg, txt_get(TXT_SMB_TRYING_MOUNT),
    config.net.server.name,
    config.serverdir
  );

  dia_info(&win_ri, msg);

  rc = net_mount_smb();

  win_close(&win_ri);

  if(rc) return -1;

  bootmode_ig = BOOTMODE_SMB;
  return 0;
}


int inst_choose_yast_version()
{
  int yast1, yast2;
  char yast1_file[MAX_FILENAME], yast2_file[MAX_FILENAME];
  static int last_item = 0;
  char *items[] = {
    txt_get(TXT_YAST1),
    txt_get(TXT_YAST2),
    NULL
  };

  *yast1_file = *yast2_file = 0;
  if(config.instsys) {
    strcpy(yast1_file, config.instsys);
    strcpy(yast2_file, config.instsys);
  }
  strcat(yast1_file, YAST1_COMMAND);
  strcat(yast2_file, YAST2_COMMAND);

  yast1 = util_check_exist(yast1_file);
  yast2 = util_check_exist(yast2_file);

  if(!yast_version_ig && auto_ig) yast_version_ig = 1;

  if(yast_version_ig == 1 && yast1) return 0;

  if(yast_version_ig == 2 && yast2) return 0;

  if(yast1 && !yast2) {
    yast_version_ig = 1;
    return 0;
  }

  if(!yast1 && yast2) {
    yast_version_ig = 2;
    return 0;
  }

  if(auto2_ig) {
    util_manual_mode();
    util_disp_init();
  }

  yast_version_ig = dia_list(txt_get(TXT_CHOOSE_YAST), 30, NULL, items, last_item, align_center);
  if(yast_version_ig) last_item = 0;

  return yast_version_ig ? 0 : -1;
}


#ifdef USE_LIBHD

int inst_auto2_install()
{
  int i;

  deb_msg("going for automatic install");

  if(ramdisk_ig) {
    deb_msg("using RAM disk");

    i = root_load_rootimage(inst_rootimage_tm);
    fprintf(stderr, "Loading of rootimage returns %d\n", i);
//    umount(mountpoint_tg);
//    umount(inst_mountpoint_tg);
    inst_umount();

    if(i || inst_rescue_im) return i;

    mkdir(inst_mountpoint_tg, 0777);
    i = util_try_mount(RAMDISK_2, inst_mountpoint_tg, MS_MGC_VAL | MS_RDONLY, 0);
    fprintf(stderr, "Mounting of %s returns %d\n", inst_mountpoint_tg, i);
    if(i) return i;
  }

  return inst_execute_yast();
}

#endif	/* USE_LIBHD */


int inst_update_cd()
{
  int  i, cdroms = 0;
  char *mp = "/tmp/drvupdate";
  hd_data_t *hd_data;
  hd_t *hd0, *hd;
  window_t win;

  *driver_update_dir = 0;

  mkdir(mp, 0755);

  dia_message("Please insert the Driver Update CD-ROM", MSGTYPE_INFO);
  dia_info(&win, "Trying to mount the CD-ROM...");	// TXT_TRY_CD_MOUNT

  hd_data = calloc(1, sizeof *hd_data);
  hd0 = hd_list(hd_data, hw_cdrom, 1, NULL);

  for(hd = hd0; hd; hd = hd->next) {
    if(hd->unix_dev_name) {
      cdrom_drives++;
      i = util_try_mount(hd->unix_dev_name, mp, MS_MGC_VAL | MS_RDONLY, 0);
      if(!i) {
        cdroms++;
        deb_msg("Update CD mounted");
        util_chk_driver_update(mp);
        umount(mp);
      }
      else {
        deb_msg("Update CD mount failed");
      }
    }
    if(*driver_update_dir) break;
  }

  hd_free_hd_list(hd0);
  hd_free_hd_data(hd_data);
  free(hd_data);

  win_close(&win);

  if(!*driver_update_dir) {
    dia_message(cdroms ? "Driver Update failed" : "Could not mount the CD-ROM", MSGTYPE_ERROR);
  }
  else {
    dia_message("Driver Update ok", MSGTYPE_INFO);
  }

  return 0;
}


void inst_swapoff()
{
  file_t *f0, *f;

  f0 = file_read_file("/proc/swaps");

  for(f = f0; f; f = f->next) {
    if(f->key == key_none && *f->key_str == '/') {
      fprintf(stderr, "swapoff %s\n", f->key_str);
      swapoff(f->key_str);
    }
  }

  file_free_file(f0);
}

