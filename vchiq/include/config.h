/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef VCHIQ_DEVICE_CONFIG_H
#define VCHIQ_DEVICE_CONFIG_H

/* These are overridden by cmake */
#ifndef DEVICE_NAME
#define DEVICE_NAME "vchiq.device"
#endif
/* The version comes from CMake (the component's one version, root CMakeLists.txt); an editor's parser gets placeholders. */
#if !defined(DEVICE_VERSION) || !defined(DEVICE_REVISION) || !defined(DEVICE_IDSTRING)
#ifdef __INTELLISENSE__
#ifndef DEVICE_VERSION
#define DEVICE_VERSION 0
#endif
#ifndef DEVICE_REVISION
#define DEVICE_REVISION 0
#endif
#ifndef DEVICE_IDSTRING
#define DEVICE_IDSTRING "vchiq.device"
#endif
#else
#error "DEVICE_VERSION, DEVICE_REVISION and DEVICE_IDSTRING are set by CMake"
#endif
#endif
#ifndef DEVICE_PRIORITY
#define DEVICE_PRIORITY 0
#endif

#define UNIT_TASK_NAME "VCHIQ VPU transport"
#define UNIT_TASK_PRIORITY 25 /* above applications, below xhci.device's 30 */
#define STACK_SIZE 16384
#define UNIT_POLL_MS 2 /* the round's period without a doorbell interrupt (BCM2836/7) */
#define PERF_REPORT_US 2000000u /* the unit task reports the bulk path's timings at most this often (profile tier) */

#define VCHIQ_WAIT_MS 2000 /* the VPU's answer to an open or a close */
#define VCHIQ_BRINGUP_US 2000000u

#endif /* VCHIQ_DEVICE_CONFIG_H */
