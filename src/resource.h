/* DriveMonitor - resource IDs. GPL-2.0-or-later. See LICENSE. */
#pragma once
#ifndef RESOURCE_H
#define RESOURCE_H

#define IDI_APPICON     101
#define IDR_VERSION     1

#define DRIVEMONITOR_VERSION         "2.0"
#define DRIVEMONITOR_BUILD           4
#define DRIVEMONITOR_BUILD_STR       "4"
#define DRIVEMONITOR_FILEVERSION     2,0,0,4
#define DRIVEMONITOR_FILEVERSION_STR "2.0 beta 4"

/* Tag that holds the source of the published GPL exe.
 * 1.7.23 is the stable release and is not this tag.
 * Change this when a newer GPL exe is published, to that tag. */
#define DRIVEMONITOR_SOURCE_TAG "2.0-beta-4"
#define DRIVEMONITOR_SOURCE_URL "https://github.com/chuikoff/DriveMonitor/tree/" DRIVEMONITOR_SOURCE_TAG

#define IDC_ADAPTER_LABEL   1027
#define IDC_ADAPTER_STATIC  1028
#define IDC_BRAND_LABEL         1029
#define IDC_BRAND_STATIC        1030
#define IDC_CONTROLLER_LABEL    1031
#define IDC_CONTROLLER_STATIC   1032
#define IDC_POH_LABEL            1036
#define IDC_POH_STATIC           1037
#define IDC_AXIS_MEDIA_L         1040
#define IDC_AXIS_MEDIA_V         1041
#define IDC_AXIS_IFACE_L         1042
#define IDC_AXIS_IFACE_V         1043
#define IDC_AXIS_TEMPA_L         1044
#define IDC_AXIS_TEMPA_V         1045
#define IDC_AXIS_ROW4_L          1046
#define IDC_AXIS_ROW4_V          1047

#endif
