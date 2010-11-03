#include <stdio.h>
#include <eegpanel.h>
#include <eegdev.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <xdfio.h>
#include <errno.h>
#include <getopt.h>

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include "configuration.h"

/**************************************************************************
 *                                                                        *
 *              Channel selection structures                              *
 *                                                                        * 
 **************************************************************************/
/*struct channel_option {
	int type;
	const char* string;
	unsigned int numch;
};

struct channel_option eeg_options[] = {
};
#define NUM_EEG_OPTS (sizeof(eeg_options)/sizeof(eeg_options[0]))

struct channel_option exg_options[] = {
};
#define NUM_EXG_OPTS (sizeof(exg_options)/sizeof(exg_options[0]))

const struct channel_option* eeg_opt = eeg_options+2;
const struct channel_option* exg_opt = exg_options+1;
*/


/**************************************************************************
 *                                                                        *
 *              Global variables                                          *
 *                                                                        * 
 **************************************************************************/
#define BIOSEMI_SYSTEM	0
#define EEGFILE_SYSTEM	1
#define GTEC_SYSTEM	2
#define NEUROSKY_SYSTEM	3
int system_used = BIOSEMI_SYSTEM;
const char* uifilename = NULL;
const char* eegfilename = NULL;
char settingsfilename[256] = "~/.eegview";
char eegset[32] = "AB";
char sensorset[32] = "all";

pthread_t thread_id;
pthread_mutex_t sync_mtx = PTHREAD_MUTEX_INITIALIZER;
struct eegdev* dev = NULL;
struct xdf* xdf = NULL;
int run_eeg = 0;
int record_file = 0;
#define NSAMPLES	32
struct systemcap info;
struct PanelSettings settings = {
	.uifilename = NULL,
	
};

size_t strides[3];
struct grpconf grp[] = {
	{
		.sensortype = EGD_EEG,
		.index = 0,
		.iarray = 0,
		.arr_offset = 0,
		.datatype = EGD_FLOAT
	},
	{
		.sensortype = EGD_SENSOR,
		.index = 0,
		.iarray = 1,
		.arr_offset = 0,
		.datatype = EGD_FLOAT
	},
	{
		.sensortype = EGD_TRIGGER,
		.index = 0,
		.iarray = 2,
		.arr_offset = 0,
		.datatype = EGD_INT32
	}
};

int StopRecording(void* user_data);
/**************************************************************************
 *                                                                        *
 *              Error message helper functions                            *
 *                                                                        * 
 **************************************************************************/
const char* get_acq_msg(int error)
{
	return strerror(error);
}

static char bdffile_message[128];
/**************************************************************************
 *                                                                        *
 *              Acquition system callbacks                                *
 *                                                                        * 
 **************************************************************************/
struct eegdev* open_eeg_device(void)
{
	if (system_used == BIOSEMI_SYSTEM)
		return egd_open_biosemi();
	else if (system_used == EEGFILE_SYSTEM)
		return egd_open_file(eegfilename);
	else if (system_used == NEUROSKY_SYSTEM)
		return egd_open_neurosky(
			"/dev/rfcomm0"
			//"/homes/nbourdau/prog/eegview/build/neurosky.bin"
			);
	else
		return NULL;
}


void* display_bdf_error(void* arg)
{
	EEGPanel* pan = arg;
	eegpanel_popup_message(pan, bdffile_message);
	return NULL;
}

int device_connection(void)
{
	if (!(dev = open_eeg_device()))
		return errno;

	// Set the number of channels of the "All channels" values
	egd_get_cap(dev, &info);
	
	grp[0].nch = settings.num_eeg < info.eeg_nmax ?
	             settings.num_eeg : info.eeg_nmax;
	grp[1].nch = settings.num_sensor < info.sensor_nmax ?
	             settings.num_sensor : info.sensor_nmax;
	grp[2].nch = info.trigger_nmax ? 1 : 0;
	strides[0] = grp[0].nch * sizeof(float);
	strides[1] = grp[1].nch * sizeof(float);
	strides[2] = grp[2].nch * sizeof(int32_t);
	return 0;
}

int set_settings_from_device(struct PanelSettings* settings)
{
	unsigned int i;
	char** labels;
	char value[32];
	free_configuration(settings);

	labels = calloc(info.eeg_nmax+1, sizeof(char*));
	if (labels == NULL)
		return 0;
	
	// Allocate and copy each labels
	for (i=0; i<info.eeg_nmax; i++) {
		egd_channel_info(dev, EGD_EEG, i, EGD_LABEL, value,EGD_EOL);
		labels[i] = malloc(strlen(value)+1);
		strcpy(labels[i], value);
	}
	settings->eeglabels = (const char**)labels;
	settings->num_eeg = info.eeg_nmax;

	labels = calloc(info.sensor_nmax+1, sizeof(char*));
	if (labels == NULL)
		return 0;
	
	// Allocate and copy each labels
	for (i=0; i<info.sensor_nmax; i++) {
		egd_channel_info(dev, EGD_SENSOR, i, EGD_LABEL, value,EGD_EOL);
		labels[i] = malloc(strlen(value)+1);
		strcpy(labels[i], value);
	}
	settings->sensorlabels = (const char**)labels;
	settings->num_sensor = info.sensor_nmax;
}

int device_disconnection(void)
{
	egd_close(dev);
	return 0;
}

// EEG acquisition thread
void* reading_thread(void* arg)
{
	float *eeg, *exg;
	int32_t *tri;
	EEGPanel* panel = arg;
	unsigned int neeg, nexg, ntri;
	int run_acq, saving, error;
	ssize_t nsread;

	neeg = grp[0].nch;
	nexg = grp[1].nch;
	ntri = grp[2].nch;

	eeg = neeg ? calloc(neeg*NSAMPLES, sizeof(*eeg)) : NULL;
	exg = nexg ? calloc(nexg*NSAMPLES, sizeof(*exg)) : NULL;
	tri = ntri ? calloc(ntri*NSAMPLES, sizeof(*tri)) : NULL;
	
	egd_start(dev);
	while (1) {
		
		// update control flags
		pthread_mutex_lock(&sync_mtx);
		run_acq = run_eeg;
		saving = record_file;
		pthread_mutex_unlock(&sync_mtx);

		// Check the stop acquisition flag
		if (!run_acq)
			break;

		// Get data from the system
		nsread = egd_get_data(dev, NSAMPLES, eeg, exg, tri);
		if (nsread < 0) {
			error = errno;			
			eegpanel_notify(panel, DISCONNECTED);
			eegpanel_popup_message(panel, get_acq_msg(error));
			break;
		}

		// Write samples on file
		if (saving) {
			if (xdf_write(xdf, nsread, eeg, exg, tri) < 0) {
				pthread_attr_t attr;
				pthread_t thid;
			
				StopRecording(NULL);
				sprintf(bdffile_message,"XDF Error: %s",strerror(errno));
				pthread_attr_init(&attr);
				pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
				pthread_create(&thid, &attr, display_bdf_error, panel);
				pthread_attr_destroy(&attr);
			}
		}

		
		eegpanel_add_samples(panel, eeg, exg, (uint32_t*)tri, NSAMPLES);
	}
	egd_stop(dev);

	free(eeg);
	free(exg);
	free(tri);

	return 0;
}

// Connection to the system 
int Connect(EEGPanel* panel)
{
	int retval;

	// Set the acquisition according to the settings
	if (egd_acq_setup(dev, 3, strides, 3, grp)) {
		retval = errno;
		egd_close(dev);
		return retval;
	}

	// Setup the panel with the settings
	eegpanel_define_input(panel, settings.num_eeg, settings.num_sensor, 
					0, info.sampling_freq);

	run_eeg = 1;
	pthread_create(&thread_id, NULL, reading_thread, panel);
	
	return 0;
}


int Disconnect(EEGPanel* panel)
{
	(void)panel;

	pthread_mutex_lock(&sync_mtx);
	run_eeg = 0;
	pthread_mutex_unlock(&sync_mtx);

	pthread_join(thread_id, NULL);
	return 0;
}


int SystemConnection(int start, void* user_data)
{
	EEGPanel* panel = user_data;
	int retval;

	retval = start ? Connect(panel) : Disconnect(panel);
	if (retval)
		eegpanel_popup_message(panel, get_acq_msg(retval));

	return (retval < 0) ? 0 : 1;
}

/**************************************************************************
 *                                                                        *
 *              File recording callbacks                                  *
 *                                                                        * 
 **************************************************************************/
int setup_xdf_channel_group(int igrp)
{
	char tmpstr[64], label[32], transducter[128], unit[16];
	double mm[2];
	unsigned int j;
	int isint;
	struct xdfch* ch;

	egd_channel_info(dev, grp[igrp].sensortype, 0,
			 EGD_UNIT, unit,
			 EGD_TRANSDUCTER, transducter,
			 EGD_MM_D, mm,
			 EGD_ISINT, &isint,
			 EGD_EOL);

	xdf_set_conf(xdf, 
	               XDF_CF_ARRINDEX, igrp,
		       XDF_CF_ARROFFSET, 0,
		       XDF_CF_ARRDIGITAL, 0,
		       XDF_CF_ARRTYPE, isint ? XDFINT32 : XDFFLOAT,
		       XDF_CF_PMIN, mm[0],
		       XDF_CF_PMAX, mm[1],
		       XDF_CF_TRANSDUCTER, transducter,
		       XDF_CF_UNIT, unit,
		       XDF_NOF);

	for (j = 0; j < grp[igrp].nch; j++) {
		egd_channel_info(dev, grp[0].sensortype, j,
		                 EGD_LABEL, label, EGD_EOL);

		// Add the channel to the BDF
		if ((ch = xdf_add_channel(xdf, NULL)) == NULL)
			return -1;

		xdf_set_chconf(ch, XDF_CF_LABEL, label, XDF_NOF);
	}
	return 0;
}

int SetupRecording(const ChannelSelection * eeg_sel,
		   const ChannelSelection * exg_sel, void *user_data)
{
	(void)eeg_sel;
	(void)exg_sel;
	EEGPanel *panel = user_data;
	char *filename;
	unsigned int j;
	size_t arrstrides[3] = {grp[0].nch*sizeof(float),
	                        grp[1].nch*sizeof(float),
				grp[2].nch*sizeof(uint32_t)};

	filename =
	    eegpanel_open_filename_dialog(panel, "BDF files|*.bdf|*.BDF||Any files|*");
	
	// Check that user hasn't press cancel
	if (!filename)
		return 0;

	// Create the BDF file
	xdf = xdf_open(filename, XDF_WRITE, XDF_BDF);
	if (!xdf) 
		goto abort;

	// Configuration file genral header
	xdf_set_conf(xdf,
	             XDF_F_REC_DURATION, 1.0,
	             XDF_F_REC_NSAMPLE, info.sampling_freq,
		     XDF_NOF);

	// Set up the channels
	for (j=0; j<3; j++)	
		if (setup_xdf_channel_group(j))
			goto abort;

	// Make the file ready for recording
	xdf_define_arrays(xdf, 3, arrstrides);
	if (xdf_prepare_transfer(xdf))
		goto abort;

	return 1;
	
abort:
	sprintf(bdffile_message,"XDF Error: %s",strerror(errno));
	eegpanel_popup_message(panel, bdffile_message);
	xdf_close(xdf);
	return 0;
}

int StopRecording(void* user_data)
{
	(void)user_data;
	pthread_mutex_lock(&sync_mtx);
	record_file = 0;
	pthread_mutex_unlock(&sync_mtx);
	
	xdf_close(xdf);
	xdf = NULL;
	return 1;
}

int ToggleRecording(int start, void* user_data)
{
	(void)user_data;
	pthread_mutex_lock(&sync_mtx);
	record_file = start;
	pthread_mutex_unlock(&sync_mtx);
	return 1;
}

/**************************************************************************
 *                                                                        *
 *              Initialization of the application                         *
 *                                                                        * 
 **************************************************************************/
enum option_index {
	SETTINGS = 0,
	UIFILE,
	EEGSET,
	SENSORSET,
	BIOSEMI,
	FILESRC,
	NEUROSKY,
	GTEC,
	SOFTWAREVERSION,
	HELP,
	NUM_OPTS
};

static struct option opt_str[] = {
	[SETTINGS] = {"settings", 1, NULL, 0},
	[UIFILE] = {"ui-file", 1, NULL, 0},
	[EEGSET] = {"eeg-set", 1, NULL, 0},
	[SENSORSET] = {"sensor-set", 1, NULL, 0},
	[BIOSEMI] = {"biosemi", 0, &system_used, BIOSEMI_SYSTEM},
	[FILESRC] = {"filesrc", 1, &system_used, EEGFILE_SYSTEM},
	[NEUROSKY] = {"neurosky", 0, &system_used, NEUROSKY_SYSTEM},
	[GTEC] = {"gtec", 0, &system_used, GTEC_SYSTEM},
	[SOFTWAREVERSION] = {"version", 0, NULL, 0},
	[HELP] = {"help", 0, NULL, 'h'}
};


static void print_usage(const char* cmd)
{
	fprintf(stdout,
"Usage: %s [GTK+ OPTIONS...]\n"
"            [--settings=FILE] [--ui-file=FILE]\n"
"            [--eeg-set=EEG_SET] [--sensor-set=SENSOR_SET]\n"
"            [--biosemi | --filesrc=FILE | --gtec | --neurosky]\n"
"            [--version] [--help | -h]\n",
               cmd);
	
}

static void print_version(void)
{
	printf("%s: version %s build on\n\t%s\n\t%s\n",
	       PACKAGE_NAME, 
	       PACKAGE_VERSION,
	       egd_get_string(),
	       xdf_get_string());
}

static int process_options(int argc, char* argv[])
{
	int c;
	int option_index = 0;

	while (1) {
		c = getopt_long(argc, argv, "h", opt_str, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 0:
			if (option_index == SETTINGS)
				strncpy(settingsfilename, optarg,
				        sizeof(settingsfilename)-1);
			else if (option_index == UIFILE)
				uifilename = optarg;
			else if (option_index == EEGSET)
				strncpy(eegset, optarg, sizeof(eegset)-1);
			else if (option_index == SENSORSET)
				strncpy(sensorset, optarg,
				        sizeof(sensorset)-1);
			else if (option_index == FILESRC)
				eegfilename = optarg;
			else if (option_index == SOFTWAREVERSION)
				print_version();
			break;

		case 'h':
			print_usage(argv[0]);
			return 1;

		case '?':
			fprintf(stderr, "?? unknown option\n");
			return -1;
		}
	}

	return 0;
}


int main(int argc, char* argv[])
{
	EEGPanel* panel = NULL;
	int retval = 0, retcode = EXIT_SUCCESS;
	struct PanelCb cb = {
		.user_data = NULL,
		.system_connection = SystemConnection,
		.setup_recording = SetupRecording,
		.stop_recording = StopRecording,
		.toggle_recording = ToggleRecording,
	};

	// Process command line options
	init_eegpanel_lib(&argc, &argv);
	retval = process_options(argc, argv);
	if (retval)
		return (retval > 0) ? 0 : -retval;
	
	read_configuration(&settings, eegset, sensorset, settingsfilename);

	if (device_connection()) {
		fprintf(stderr, "Device connection failed (%i): %s\n",
				errno, strerror(errno));
		retcode = EXIT_FAILURE;
		goto exit;
	}
	set_settings_from_device(&settings);

	settings.uifilename = uifilename;
	panel = eegpanel_create(&settings, &cb);
	if (!panel) {
		fprintf(stderr,"error at the creation of the panel\n");
		retcode = EXIT_FAILURE;
		goto exit;
	}
	
	// Run the panel
	eegpanel_show(panel, 1);
	eegpanel_run(panel, 0);

	eegpanel_destroy(panel);
exit:
	device_disconnection();
	free_configuration(&settings);
	

	return retcode;
}

