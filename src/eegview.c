#include <stdio.h>
#include <eegpanel.h>
#include <act2demux.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <xdfio.h>
#include <errno.h>

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include "configuration.h"

/**************************************************************************
 *                                                                        *
 *              Channel selection structures                              *
 *                                                                        * 
 **************************************************************************/
struct channel_option {
	int type;
	const char* string;
	unsigned int numch;
};

struct channel_option eeg_options[] = {
	{ALLEEG, "all", -1},
	{A, "A", 32},
	{AB, "AB", 64},
	{AD, "AD", 128}
};
#define NUM_EEG_OPTS (sizeof(eeg_options)/sizeof(eeg_options[0]))

struct channel_option exg_options[] = {
	{ALLEXG, "all", -1},
	{STD, "STD", 8},
	{NONE, "none", 0}
};
#define NUM_EXG_OPTS (sizeof(exg_options)/sizeof(exg_options[0]))

const struct channel_option* eeg_opt = eeg_options+2;
const struct channel_option* exg_opt = exg_options+1;



/**************************************************************************
 *                                                                        *
 *              Global variables                                          *
 *                                                                        * 
 **************************************************************************/
pthread_t thread_id;
pthread_mutex_t sync_mtx = PTHREAD_MUTEX_INITIALIZER;
struct xdf* xdf = NULL;
int run_eeg = 0;
int record_file = 0;
#define NSAMPLES	32
ActivetwoSystemInfo info;
struct PanelSettings settings = {
	.uifilename = NULL
};

int StopRecording(void* user_data);
/**************************************************************************
 *                                                                        *
 *              Error message helper functions                            *
 *                                                                        * 
 **************************************************************************/
static char* const  acquisition_system_message[] = {
	"Attempt to open the driver failed.\n\nCheck that it is correctly installed, or that the USB cable is plugged.",
	"The synchronization with the ADC box is lost.\n\nCheck that the ADC box is switched on or that the optical cable is plugged.",
	"An attempt to get the data in buffered mode has been made, but the acquisition has not been started yet.",
	"The buffer is full. Data has been lost",
	"The specified option is not supported or is not consistent with the capabilities of the system"
};
#define NUM_SYS_MESSAGE	(sizeof(acquisition_system_message)/sizeof(char*))

const char* get_acq_message(ActivetwoRetCode retcode)
{
	unsigned int msg_ind  = SUCCESS - retcode -1;

	if (msg_ind > NUM_SYS_MESSAGE)
		return "Unknown error.";
	return acquisition_system_message[msg_ind];
}

static char bdffile_message[128];
/**************************************************************************
 *                                                                        *
 *              Acquition system callbacks                                *
 *                                                                        * 
 **************************************************************************/
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
	uint32_t *tri;
	int32_t *raweeg, *rawexg;
	EEGPanel* panel = arg;
	unsigned int neeg, nexg;
	ActivetwoRetCode ret;
	int run_acq, saving;

	neeg = eeg_opt->numch;
	nexg = exg_opt->numch;

	eeg = calloc(neeg*NSAMPLES, sizeof(*eeg));
	exg = calloc(nexg*NSAMPLES, sizeof(*exg));
	tri = calloc(NSAMPLES, sizeof(*tri));
	raweeg = (int32_t*)eeg;
	rawexg = (int32_t*)exg;
	
	ActivetwoStartBufferedAcquisition();
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
		ret = ActivetwoGetBufferedSamples(NSAMPLES, tri, raweeg, rawexg);
		if (ret != SUCCESS) {
			eegpanel_notify(panel, DISCONNECTED);
			eegpanel_popup_message(panel, get_acq_message(ret));
			break;
		}

		// Scale data
		Act2ScaleArray(float, eeg, int32_t, raweeg, NSAMPLES*neeg, ACTIVE_ELEC_SCALE);
		Act2ScaleArray(float, exg, int32_t, rawexg, NSAMPLES*nexg, ACTIVE_ELEC_SCALE);
		Act2ScaleArray(uint32_t, tri, uint32_t, tri, NSAMPLES, TRIGGER_SCALE);

		// Write samples on file
		if (saving) {
			if (xdf_write(xdf, NSAMPLES, eeg, exg, tri) < 0) {
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

		
		eegpanel_add_samples(panel, eeg, exg, tri, NSAMPLES);
	}
	ActivetwoStopBufferedAcquisition();

	free(eeg);
	free(exg);
	free(tri);

	return 0;
}

// Connection to the system 
int Connect(EEGPanel* panel)
{
	int retval;
	
	if ((retval = ActivetwoConnectToSystem()) < 0)
		return retval;

	// Set the number of channels of the "All channels" values
	info = ActivetwoGetSystemInfo();
	eeg_options[0].numch = info.maxnumber_eeg_channels;
	exg_options[0].numch = info.maxnumber_sensor_channels;
	

	// Set the acquisition according to the settings
	if ((retval = ActivetwoSetDataFormats(eeg_opt->type, exg_opt->type))!=SUCCESS) {
		ActivetwoDisconnectFromSystem();
		return retval;
	}

	// Setup the panel with the settings
	eegpanel_define_input(panel, eeg_opt->numch, exg_opt->numch, 16, info.samplerate);

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
	ActivetwoDisconnectFromSystem();
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
	unsigned int arrstrides[3] = {eeg_opt->numch*sizeof(float),
	                        exg_opt->numch*sizeof(float),
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
	             XDF_F_REC_NSAMPLE, info.samplerate,
		     XDF_NOF);
	for (j = 0; j < eeg_opt->numch; j++) {
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
	for (j = 0; j < exg_opt->numch; j++) {
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
		       XDF_CF_ARRTYPE, XDFUINT32,
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
const char* opt_str[] = {
	"--settings=",
	"--ui-file=",
	"--map-file=",
	"--eeg-set=",
	"--exg-set="
};
#define NUM_OPTS	(sizeof(opt_str)/sizeof(opt_str[0]))

const struct channel_option* find_chann_opt(const struct channel_option options[], unsigned int num_opt, const char* string)
{
	unsigned int i;
	const struct channel_option* chann_opt = NULL;

	for (i=0; i<num_opt; i++) {
		if (strcmp(options[i].string, string)==0) {
			chann_opt = options + i;
			break;
		}
	}

	return chann_opt;
}


int main(int argc, char* argv[])
{
	EEGPanel* panel = NULL;
	int i, retval = 0;
	unsigned int iopt;
	const char* opt_vals[NUM_OPTS] = {NULL};
	struct PanelCb cb = {
		.user_data = NULL,
		.system_connection = SystemConnection,
		.setup_recording = SetupRecording,
		.stop_recording = StopRecording,
		.toggle_recording = ToggleRecording,
	};

	// Process command line options
	init_eegpanel_lib(&argc, &argv);
	for (i=1; i<argc; i++) {
		for (iopt=0; iopt<NUM_OPTS; iopt++) {
			if (strncmp(opt_str[iopt],argv[i],strlen(opt_str[iopt]))==0) {
				opt_vals[iopt] = argv[i]+strlen(opt_str[iopt]);
				break;
			}
		}
		// The option has been found so move to the next 
		if (iopt<NUM_OPTS)
			continue;

		// Print version of software and its dependencies
		if (strcmp(argv[i],"--version")==0) {
			printf("%s: version %s build on\n\t%s\n\t%s\n",argv[0], PACKAGE_VERSION, ActivetwoGetString(),xdf_get_string());
			return 0;
		}

		// Print usage string in case of unknown option
		// or if "--help" or "-h" is provided
		if ( (iopt == NUM_OPTS)
		     && (strcmp(argv[i], "--help")!=0)
		     && (strcmp(argv[i], "-h")!=0) ) {
		     	fprintf(stderr, "Unknown option\n");
			retval = 1;
		}
		printf("Usage: %s [GTK+ OPTIONS...]\n"
		       "	    [--settings=FILE] [--map-file=FILE]\n"
		       "            [--ui-file=FILE] [--eeg-set=EEG_SET]\n"
		       "            [--exg-set=EXG_SET] [--version]\n",argv[0]);

		return retval;
	}
	
	// Process EEG channels specification
	if (opt_vals[3]) {
		eeg_opt = find_chann_opt(eeg_options, NUM_EEG_OPTS, opt_vals[3]);
		if (!eeg_opt) {
			fprintf(stderr, "invalid option for EEG (%s)\n",opt_vals[3]);
			return 1;
		}
	}
	
	// Process EXG channels specification
	if (opt_vals[4]) {
		exg_opt = find_chann_opt(exg_options, NUM_EXG_OPTS, opt_vals[4]);
		if (!exg_opt) {
			fprintf(stderr, "invalid option for EXG (%s)\n",opt_vals[4]);
			return 1;
		}
	}
	
	read_configuration(&settings, eeg_opt->string, exg_opt->string);


	settings.uifilename = opt_vals[1];
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

