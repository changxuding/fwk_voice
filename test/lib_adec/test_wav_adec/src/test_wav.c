#include "xs3_math.h"
#include "fileio.h"
#include "wav_utils.h"
#include "aec_config.h"
#include "pipeline_state.h"

#ifndef LOG_DEBUG_INFO 
    #define LOG_DEBUG_INFO (0)
#endif
#define AP_MAX_Y_CHANNELS (2)
#define AP_MAX_X_CHANNELS (2)
#define AP_FRAME_ADVANCE (240)

extern void pipeline_init(pipeline_state_t *state, aec_conf_t *de_conf, aec_conf_t *non_de_conf);
extern void pipeline_process_frame(pipeline_state_t *state,
    int32_t (*output_data)[AP_FRAME_ADVANCE],
    int32_t (*input_y_data)[AP_FRAME_ADVANCE],
    int32_t (*input_x_data)[AP_FRAME_ADVANCE]);

#define ARG_NOT_SPECIFIED (-1)
typedef enum {
    Y_CHANNELS,
    X_CHANNELS,
    MAIN_FILTER_PHASES,
    SHADOW_FILTER_PHASES,
    NUM_RUNTIME_ARGS
}runtime_args_indexes_t;

int runtime_args[NUM_RUNTIME_ARGS];
//valid_tokens_str entries and runtime_args_indexes_t need to maintain the same order so that when a runtime argument token string matches index 'i' string in valid_tokens_str, the corresponding
//value can be updated in runtime_args[i]
const char *valid_tokens_str[] = {"y_channels", "x_channels", "main_filter_phases", "shadow_filter_phases"}; //TODO autogenerate from runtime_args_indexes_t

#define MAX_ARGS_BUF_SIZE (1024)
void parse_runtime_args(int *runtime_args_arr) {
    file_t args_file;
    int ret = file_open(&args_file, "args.bin", "rb");
    if(ret != 0) {
        return;
    }
    char readbuf[MAX_ARGS_BUF_SIZE];

    int args_file_size = get_file_size(&args_file);
    printf("args_file_size = %d\n",args_file_size);
    if(!args_file_size) {
        file_close(&args_file);
        return;
    }
    file_read(&args_file, (uint8_t*)readbuf, args_file_size);
    readbuf[args_file_size] = '\0';

    //printf("args %s\n",readbuf);
    char *c = strtok(readbuf, "\n");
    while(c != NULL) {
        char token_str[100];
        int token_val;
        sscanf(c, "%s %d", token_str, &token_val);
        //printf("token %s\n",c);
        for(int i=0; i<sizeof(valid_tokens_str)/sizeof(valid_tokens_str[0]); i++) {
            if(strcmp(valid_tokens_str[i], token_str) == 0) {
                //printf("found token %s, value %d\n", valid_tokens_str[i], token_val);
                runtime_args_arr[i] = token_val;
            }
        }
        //printf("str %s val %d\n",token_str, token_val);
        c = strtok(NULL, "\n");
    }
}

void stage_a_wrapper(const char *input_file_name, const char* output_file_name)
{
    //check validity of compile time configuration
    assert(AEC_MAX_Y_CHANNELS <= AEC_LIB_MAX_Y_CHANNELS);
    assert(AEC_MAX_X_CHANNELS <= AEC_LIB_MAX_X_CHANNELS);
    assert((AEC_MAX_Y_CHANNELS * AEC_MAX_X_CHANNELS * AEC_MAIN_FILTER_PHASES) <= (AEC_LIB_MAX_PHASES));
    assert((AEC_MAX_Y_CHANNELS * AEC_MAX_X_CHANNELS * AEC_SHADOW_FILTER_PHASES) <= (AEC_LIB_MAX_PHASES));
    //Initialise default values of runtime arguments
    runtime_args[Y_CHANNELS] = AEC_MAX_Y_CHANNELS;
    runtime_args[X_CHANNELS] = AEC_MAX_X_CHANNELS;
    runtime_args[MAIN_FILTER_PHASES] = AEC_MAIN_FILTER_PHASES;
    runtime_args[SHADOW_FILTER_PHASES] = AEC_SHADOW_FILTER_PHASES;

    parse_runtime_args(runtime_args);
    printf("runtime args = ");
    for(int i=0; i<NUM_RUNTIME_ARGS; i++) {
        printf("%d ",runtime_args[i]);
    }
    printf("\n");

    //Check validity of runtime configuration
    assert(runtime_args[Y_CHANNELS] <= AEC_MAX_Y_CHANNELS);
    assert(runtime_args[X_CHANNELS] <= AEC_MAX_X_CHANNELS);
    assert((runtime_args[Y_CHANNELS] * runtime_args[X_CHANNELS] * runtime_args[MAIN_FILTER_PHASES]) <= (AEC_LIB_MAX_PHASES));
    assert((runtime_args[Y_CHANNELS] * runtime_args[X_CHANNELS] * runtime_args[SHADOW_FILTER_PHASES]) <= (AEC_LIB_MAX_PHASES));

    file_t input_file, output_file, delay_file;
    // Open input wav file containing mic and ref channels of input data
    int ret = file_open(&input_file, input_file_name, "rb");
    assert((!ret) && "Failed to open file");
    // Open output wav file that will contain the AEC output
    ret = file_open(&output_file, output_file_name, "wb");
    assert((!ret) && "Failed to open file");
    ret = file_open(&delay_file, "delay.bin", "wb");
    assert((!ret) && "Failed to open file");

#if LOG_DEBUG_INFO
    file_t debug_log_file;
    ret = file_open(&debug_log_file, "debug_log.bin", "wb");
#endif

    wav_header input_header_struct, output_header_struct;
    unsigned input_header_size;
    if(get_wav_header_details(&input_file, &input_header_struct, &input_header_size) != 0){
        printf("error in att_get_wav_header_details()\n");
        _Exit(1);
    }

    file_seek(&input_file, input_header_size, SEEK_SET);
    // Ensure 32bit wav file
    if(input_header_struct.bit_depth != 32)
     {
         printf("Error: unsupported wav bit depth (%d) for %s file. Only 32 supported\n", input_header_struct.bit_depth, input_file_name);
         _Exit(1);
     }
    // Ensure input wav file contains correct number of channels 
    if(input_header_struct.num_channels != (AP_MAX_Y_CHANNELS+AP_MAX_X_CHANNELS)){
        printf("Error: wav num channels(%d) does not match aec(%u)\n", input_header_struct.num_channels, (AP_MAX_Y_CHANNELS+AP_MAX_X_CHANNELS));
        _Exit(1);
    }
    
    unsigned frame_count = wav_get_num_frames(&input_header_struct);
    // Calculate number of frames in the wav file
    unsigned block_count = frame_count / AP_FRAME_ADVANCE;
    wav_form_header(&output_header_struct,
            input_header_struct.audio_format,
            AP_MAX_Y_CHANNELS,
            input_header_struct.sample_rate,
            input_header_struct.bit_depth,
            block_count*AP_FRAME_ADVANCE);

    file_write(&output_file, (uint8_t*)(&output_header_struct),  WAV_HEADER_BYTES);

    int32_t input_read_buffer[AP_FRAME_ADVANCE * (AP_MAX_Y_CHANNELS + AP_MAX_X_CHANNELS)] = {0}; // Array for storing interleaved input read from wav file
    int32_t output_write_buffer[AP_FRAME_ADVANCE * (AP_MAX_Y_CHANNELS)];

    int32_t DWORD_ALIGNED frame_y[AP_MAX_Y_CHANNELS][AP_FRAME_ADVANCE];
    int32_t DWORD_ALIGNED frame_x[AP_MAX_X_CHANNELS][AP_FRAME_ADVANCE];
    int32_t DWORD_ALIGNED stage_a_output[2][AP_FRAME_ADVANCE];

    unsigned bytes_per_frame = wav_get_num_bytes_per_frame(&input_header_struct);
    
    aec_conf_t aec_de_mode_conf, aec_non_de_mode_conf;
    // DE mode AEC config is fixed and not run time configurable
    aec_de_mode_conf.num_x_channels = 1;
    aec_de_mode_conf.num_y_channels = 1;
    aec_de_mode_conf.num_main_filt_phases = 30;
    aec_de_mode_conf.num_shadow_filt_phases = 0;
    
    /** Non DE mode AEC config is runtime configurable, main reason being ADEC tests pass only for alt arch (1, 2, 15,
     * 5) config while for profiling I want to use the worst case (2, 2, 10, 5) config*/
    aec_non_de_mode_conf.num_y_channels = runtime_args[Y_CHANNELS];
    aec_non_de_mode_conf.num_x_channels = runtime_args[X_CHANNELS];
    aec_non_de_mode_conf.num_main_filt_phases = runtime_args[MAIN_FILTER_PHASES];
    aec_non_de_mode_conf.num_shadow_filt_phases = runtime_args[SHADOW_FILTER_PHASES];
    
    //Initialise ap_stage_a
    pipeline_state_t DWORD_ALIGNED stage_a_state;
    pipeline_init(&stage_a_state, &aec_de_mode_conf, &aec_non_de_mode_conf);

#if LOG_DEBUG_INFO
    //bypass adec since we only want to log aec behaviour
    stage_a_state.adec_state.adec_config.bypass = 1;
#endif
    for(unsigned b=0;b<block_count;b++){
        long input_location =  wav_get_frame_start(&input_header_struct, b * AP_FRAME_ADVANCE, input_header_size);
        file_seek (&input_file, input_location, SEEK_SET);
        file_read (&input_file, (uint8_t*)&input_read_buffer[0], bytes_per_frame* AP_FRAME_ADVANCE);
        // Deinterleave and copy y and x samples to their respective buffers
        for(unsigned f=0; f<AP_FRAME_ADVANCE; f++){
            for(unsigned ch=0;ch<AP_MAX_Y_CHANNELS;ch++){
                unsigned i =(f * (AP_MAX_Y_CHANNELS+AP_MAX_X_CHANNELS)) + ch;
                frame_y[ch][f] = input_read_buffer[i];
            }
            for(unsigned ch=0;ch<AP_MAX_X_CHANNELS;ch++){
                unsigned i =(f * (AP_MAX_Y_CHANNELS+AP_MAX_X_CHANNELS)) + AP_MAX_Y_CHANNELS + ch;
                frame_x[ch][f] = input_read_buffer[i];
            }
        }

        pipeline_process_frame(&stage_a_state, stage_a_output, frame_y, frame_x);

#if LOG_DEBUG_INFO
    char buf[100];
    sprintf(buf, "%f\n", float_s32_to_float(stage_a_state.aec_main_state.overall_Error[0]));
    file_write(&debug_log_file, (uint8_t*)buf,  strlen(buf));
    sprintf(buf, "%f\n", float_s32_to_float(stage_a_state.aec_shadow_state.overall_Error[0]));
    file_write(&debug_log_file, (uint8_t*)buf,  strlen(buf));
    sprintf(buf, "%f\n", float_s32_to_float(stage_a_state.aec_main_state.shared_state->overall_Y[0]));
    file_write(&debug_log_file, (uint8_t*)buf,  strlen(buf));
    sprintf(buf, "%d\n", stage_a_state.aec_main_state.shared_state->shadow_filter_params.shadow_flag[0]);
    file_write(&debug_log_file, (uint8_t*)buf,  strlen(buf));
    sprintf(buf, "%d\n", stage_a_state.aec_main_state.shared_state->shadow_filter_params.shadow_reset_count[0]);
    file_write(&debug_log_file, (uint8_t*)buf,  strlen(buf));
    sprintf(buf, "%d\n", stage_a_state.aec_main_state.shared_state->shadow_filter_params.shadow_better_count[0]);
    file_write(&debug_log_file, (uint8_t*)buf,  strlen(buf));
    sprintf(buf, "%f\n", float_s32_to_float(stage_a_state.aec_main_state.error_ema_energy[0]));
    file_write(&debug_log_file, (uint8_t*)buf,  strlen(buf));
    sprintf(buf, "%f\n", float_s32_to_float(stage_a_state.aec_main_state.shared_state->y_ema_energy[0]));
    file_write(&debug_log_file, (uint8_t*)buf,  strlen(buf));
    sprintf(buf, "%f\n", float_s32_to_float(stage_a_state.peak_to_average_ratio));
    file_write(&debug_log_file, (uint8_t*)buf,  strlen(buf));
#endif
        
        // Create interleaved output that can be written to wav file
        for (unsigned ch=0;ch<AP_MAX_Y_CHANNELS;ch++){
            for(unsigned i=0;i<AP_FRAME_ADVANCE;i++){
                output_write_buffer[i*(AP_MAX_Y_CHANNELS) + ch] = stage_a_output[ch][i];
            }
        }

        file_write(&output_file, (uint8_t*)(output_write_buffer), output_header_struct.bit_depth/8 * AP_FRAME_ADVANCE * AP_MAX_Y_CHANNELS);
        
        char strbuf[100];
        sprintf(strbuf, "%ld\n", stage_a_state.adec_requested_delay_samples);
        file_write(&delay_file, (uint8_t*)strbuf,  strlen(strbuf));
    }
    file_close(&input_file);
    file_close(&output_file);
    file_close(&delay_file);
    shutdown_session();
}
#if X86_BUILD
int main(int argc, char **argv) {
    /*if(argc < 3) {
        printf("Arguments missing. Expected: <input file name> <output file name>\n");
        assert(0);
    }*/
    stage_a_wrapper("input.wav", "output.wav");
    //aec_task(argv[1], argv[2]);
    return 0;
}
#endif
