/* Exercise the actual patched sensor integration with synthetic timestamps.
 * Build/link instructions are in scripts/check-tracker.sh. No hardware is used. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "device_imu.h"
#include "Fusion/Fusion.h"

static uint64_t stamp;
static float turn_rate;
static int next_sample(struct device_imu_t *device, struct imu_sample *out, int timeout) {
    (void)device; (void)timeout;
    memset(out, 0, sizeof(*out));
    out->timestamp_ns = stamp;
    out->gz = turn_rate; /* A real, slow turn must remain observable. */
    out->az = 1.0f;
    out->mx = out->my = out->mz = NAN;
    out->temperature_c = NAN;
    return 1;
}

static FusionQuaternion run(uint64_t epoch) {
    const imu_protocol protocol = {.next_sample = next_sample};
    FusionAhrs ahrs;
    FusionAhrsInitialise(&ahrs);
    const FusionAhrsSettings settings = {.convention = FusionConventionNed, .gain = 0.5f};
    FusionAhrsSetSettings(&ahrs, &settings);
    device_imu_type device = {.protocol = &protocol, .ahrs = (void *)&ahrs};
    stamp = epoch;
    turn_rate = 1.0f;
    /* Test integration independently of Fusion's initial heading zeroing. */
    ahrs.initialising = false;
    ahrs.rampedGain = 0.5f;
    assert(device_imu_read(&device, 0) == DEVICE_IMU_ERROR_NO_ERROR);
    assert(fabsf(FusionAhrsGetQuaternion(&ahrs).element.z) < 0.0001f);
    for (int i = 0; i < 1000; ++i) {
        stamp += 1000000;
        assert(device_imu_read(&device, 0) == DEVICE_IMU_ERROR_NO_ERROR);
    }
    FusionQuaternion before = FusionAhrsGetQuaternion(&ahrs);
    FusionEuler euler = FusionQuaternionToEuler(before);
    assert(fabsf(euler.angle.yaw + 1.0f) < 0.02f);
    /* A pause and a backward timestamp must not teleport the orientation. */
    stamp += 600000000000ULL;
    assert(device_imu_read(&device, 0) == DEVICE_IMU_ERROR_NO_ERROR);
    assert(memcmp(&before, &ahrs.quaternion, sizeof(before)) == 0);
    stamp -= 1000000000;
    assert(device_imu_read(&device, 0) == DEVICE_IMU_ERROR_NO_ERROR);
    assert(memcmp(&before, &ahrs.quaternion, sizeof(before)) == 0);
    stamp += 1000000;
    assert(device_imu_read(&device, 0) == DEVICE_IMU_ERROR_NO_ERROR);
    return FusionAhrsGetQuaternion(&ahrs);
}

int main(void) {
    FusionQuaternion short_uptime = run(1000000000);
    FusionQuaternion long_uptime = run(600000000000ULL);
    for (int i = 0; i < 4; ++i)
        assert(fabsf(short_uptime.array[i] - long_uptime.array[i]) < 0.000001f);
    puts("PASS: orientation ignores device uptime, stream gaps and backwards stamps; slow turns remain observable.");
    return 0;
}
