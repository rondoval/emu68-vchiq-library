/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vchiq_test.h - vchiq.device's test-only commands, for the hardware test
 * suite (tests/).  Not part of the public interface: a device
 * built without VCHIQ_TEST_HOOKS (every release) answers them IOERR_NOCMD.
 *
 * NSCMD_VQ_TEST_ABANDON: the next reset leaves the session as a crash would
 * (no quiesce, the record left busy with its services), so the first open
 * after it repairs the session.  Answered at once, on any opened request.
 */
#ifndef VCHIQ_VCHIQ_TEST_H
#define VCHIQ_VCHIQ_TEST_H

#include <devices/vchiq.h>

#define NSCMD_VQ_TEST_ABANDON (NSCMD_VCHIQ_BASE + 0xff)

#endif /* VCHIQ_VCHIQ_TEST_H */
