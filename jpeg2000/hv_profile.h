/* Output guarantees shared by merge and transcode. Input readers only
 * interpret fields needed to meet the selected output requirements. */
#ifndef HV_PROFILE_H
#define HV_PROFILE_H

typedef enum {
    HV_OUTPUT_JPEG2000, /* operation's supported JPEG 2000 output; no JPIP promise */
    HV_OUTPUT_JPIP      /* also enforce the serving profile in JPIP_PROFILE.md */
} hv_output_profile;

#endif
