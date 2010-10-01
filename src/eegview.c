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
const char* get_acq_message(int error)
{
	return strerror(error);
}

static char bdffile_message[128];
/**************************************************************************
 *                                                                        *
 *              Acquition system callbacks                                *
 *                                                                        * 
 **************************************************************************/
unsigned int grpindex[EGD_NUM_STYPE] = {
	[EGD_EEG] = 0, 
	[EGD_SENSOR] = 64,
	[EGD_TRIGGER] = 72
};
struct eegdev* open_eeg_device(void)
{
	if (system_used == BIOSEMI_SYSTEM)
		return egd_open_biosemi();
	else if (system_used == EEGFILE_SYSTEM)
		return egd_open_file(eegfilename, grpindex);
	else
		return NULL;
}


void* display_bdf_error(void* arg)
{
	EEGPanel* pan = arg;
	eegpanel_popup_message(pan, bdffile_message);
	return NULL;
}

// EEG acquisition thread
void* reading_thread(void* arg)
{
	float *eeg, *exg;
	int32_t *tri;
	EEGPanel* panel = arg;
	unsigned int neeg, nexg;
	int run_acq, saving, error;
	ssize_t nsread;

	neeg = settings.num_eeg;
	nexg = settings.num_sensor;

	eeg = calloc(neeg*NSAMPLES, sizeof(*eeg));
	exg = calloc(nexg*NSAMPLES, sizeof(*exg));
	tri = calloc(NSAMPLES, sizeof(*tri));
	
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
			eegpanel_popup_message(panel, get_acq_message(error));
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
	size_t strides[3];
	
	if (!(dev = open_eeg_device()))
		return errno;

	// Set the number of channels of the "All channels" values
	egd_get_cap(dev, &info);
	
	grp[0].nch = settings.num_eeg;
	grp[1].nch = settings.num_sensor;
	grp[2].nch = 1;
	strides[0] = grp[0].nch * sizeof(float);
	strides[1] = grp[1].nch * sizeof(float);
	strides[2] = grp[2].nch * sizeof(int32_t);

	// Set the acquisition according to the settings
	if (egd_acq_setup(dev, 3, strides, 3, grp)) {
		retval = errno;
		egd_close(dev);
		return retval;
	}

	// Setup the panel with the settings
	eegpanel_define_input(panel, settings.num_eeg, settings.num_sensor, 16, info.sampling_freq);

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
	egd_close(dev);
	return 0;
}


int SystemConnection(int start, void* user_data)
{
	EEGPanel* panel = user_data;
	int retval;

	retval = start ? Connect(panel) : Disconnect(panel);
	if (retval)
		eegpanel_popup_message(panel, get_acq_message(retval));

	return (retval < 0) ? 0 : 1;
}

/**************************************************************************
 *                                                                        *
 *              File recording callbacks                                  *
 *                                                                        * 
 **************************************************************************/
int SetupRecording(const ChannelSelection * eeg_sel,
		   const ChannelSelection * exg_sel, void *user_data)
{
	(void)eeg_sel;
	(void)exg_sel;
	EEGPanel *panel = user_data;
	char *filename;
	char tmpstr[64];
	unsigned int j;
	struct xdfch* ch;
	size_t arrstrides[3] = {settings.num_eeg*sizeof(float),
	                        settings.num_sensor*sizeof(float),
				sizeof(uint32_t)};

	filename =
	    eegpanel_open_filename_dialog(panel, "BDF files|*.bdf|*.BDF||Any files|*");
	
	// Check that user hasn't press cancel
	if (!filename)
		return 0;

	// Create the BDF file
	xdf = xdf_open(filename, XDF_WRITE, XDF_BDF);
	if (!xdf) 
		goto abort;


	// Set up the channels
	xdf_set_conf(xdf,
	             XDF_F_REC_DURATION, 1.0,
	             XDF_F_REC_NSAMPLE, info.sampling_freq,
		     XDF_NOF);
	for (j = 0; j < settings.num_eeg; j++) {
		// Use labels for channel if available
		if (settings.eeglabels && j<settings.num_eeg)
			strncpy(tmpstr, settings.eeglabels[j], sizeof(tmpstr)-1);
		else
			sprintf(tmpstr, "EEG%i", j+1);

		// Add the channel to the BDF
		if ((ch = xdf_add_channel(xdf, NULL)) == NULL)
			goto abort;

		xdf_set_chconf(ch, 
		               XDF_CF_ARRINDEX, 0,
			       XDF_CF_ARROFFSET, j*sizeof(float),
			       XDF_CF_ARRDIGITAL, 0,
			       XDF_CF_ARRTYPE, XDFFLOAT,
			       XDF_CF_LABEL, tmpstr,
			       XDF_CF_PMIN, -262144.0,
			       XDF_CF_PMAX, 262143.0,
			       XDF_CF_DMIN, -8388608.0,
			       XDF_CF_DMAX, 8388607.0,
			       XDF_CF_PREFILTERING, "HP: DC; LP: 417 Hz",
			       XDF_CF_TRANSDUCTER, "Active Electrode",
			       XDF_CF_UNIT, "uV",
			       XDF_CF_RESERVED, "EEG",
			       XDF_NOF);
	}
	for (j = 0; j < settings.num_sensor; j++) {
		// Use labels for channel if available
		if (settings.sensorlabels && j<settings.num_sensor)
			strncpy(tmpstr, settings.sensorlabels[j], sizeof(tmpstr)-1);
		else
			sprintf(tmpstr, "EXG%i", j+1);

		// Add the channel to the BDF
		if ((ch = xdf_add_channel(xdf, NULL)) == NULL)
			goto abort;

		xdf_set_chconf(ch, 
		               XDF_CF_ARRINDEX, 1,
			       XDF_CF_ARROFFSET, j*sizeof(float),
			       XDF_CF_ARRDIGITAL, 0,
			       XDF_CF_ARRTYPE, XDFFLOAT,
			       XDF_CF_LABEL, tmpstr,
			       XDF_CF_PMIN, -262144.0,
			       XDF_CF_PMAX, 262143.0,
			       XDF_CF_DMIN, -8388608.0,
			       XDF_CF_DMAX, 8388607.0,
			       XDF_CF_PREFILTERING, "HP: DC; LP: 417 Hz",
			       XDF_CF_TRANSDUCTER, "Active Electrode",
			       XDF_CF_UNIT, "uV",
			       XDF_CF_RESERVED, "EXG",
			       XDF_NOF);
	}

	// Add the status to the BDF
	if ((ch = xdf_add_channel(xdf, NULL)) == NULL)
		goto abort;

	xdf_set_chconf(ch, 
	               XDF_CF_ARRINDEX, 2,
		       XDF_CF_ARROFFSET, 0,
		       XDF_CF_ARRDIGITAL, 0,
		       XDF_CF_ARRTYPE, XDFINT32,
		       XDF_CF_LABEL, "Status",
		       XDF_CF_PMIN, -8388608.0,
		       XDF_CF_PMAX, 8388607.0,
		       XDF_CF_DMIN, -8388608.0,
		       XDF_CF_DMAX, 8388607.0,
		       XDF_CF_PREFILTERING, "No filtering",
		       XDF_CF_TRANSDUCTER, "Triggers and Status",
		       XDF_CF_UNIT, "Boolean",
		       XDF_CF_RESERVED, "TRI",
		       XDF_NOF);

	// Make the file ready for recording
	xdf_define_arrays(xdf, 3, arrstrides);
	if(xdf_prepare_transfer(xdf))
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
"            [--biosemi | --filesrc=FILE | --gtec]\n"
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
					strncpy(settingsfilename, optarg, sizeof(settingsfilename)-1);
				else if (option_index == UIFILE)
					uifilename = optarg;
				else if (option_index == EEGSET)
					strncpy(eegset, optarg, sizeof(eegset)-1);
				else if (option_index == SENSORSET)
					strncpy(sensorset, optarg, sizeof(sensorset)-1);
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
	int retval = 0;
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


	settings.uifilename = uifilename;
	panel = eegpanel_create(&settings, &cb);
	if (!panel) {
		fprintf(stderr,"error at the creation of the panel\n");
		return 2;
	}
	
	// Run the panel
	eegpanel_show(panel, 1);
	eegpanel_run(panel, 0);


	eegpanel_destroy(panel);
	free_configuration(&settings);

	return 0;
}

