#!/bin/bash
#
# Run by udev when the display connection changes (60-drm.rules). Picks the
# HDMI output and its video mode, routes audio to HDMI or the headphone jack
# (AudioRoute in common.sh, shared with S50ui), and once booted makes sure
# sq-session is running so a game shows up as soon as a screen is connected.
#

source /usr/bin/av_helper.sh
source /usr/lib/retronsq/common.sh
PREFERED_CONNECTORS="hdmi internal"
PREFERED_MODES="1280x720 1024x 1280x1024 800x600 1920x1080 [0-9]"
CONNECTOR_ID=
CONNECTOR_TYPE=
CONNECTOR_MODE=

get_prefered_connector()
{
	for type in $PREFERED_CONNECTORS;do
		ID=$(drm_id_from_option $type)
		if [ "$ID" ];then
			CONNECTOR_ID=$ID
			CONNECTOR_TYPE=$type
			return
		fi
	done
}

get_prefered_mode()
{
	MODES=$(drm_get_properties $CONNECTOR_ID MODES)

	for mode in $PREFERED_MODES;do
		MATCH=$(echo $MODES|xargs -n 1|grep "^$mode"|head -1)
		if [ "$MATCH" ];then
			CONNECTOR_MODE=$MATCH
			return
		fi
	done
}

drm_init_connectors connected

get_prefered_connector
echo Prefered connector is $CONNECTOR_ID / $CONNECTOR_TYPE

get_prefered_mode
echo Prefered mode is $CONNECTOR_MODE

drm_set_connector $CONNECTOR_ID $CONNECTOR_MODE

# New /etc/asound.conf provides hdmi/internal alsa pcm configs
PLAYBACK_NAME=$CONNECTOR_TYPE
echo Prefered playback is $PLAYBACK_NAME
alsa_set_playback $PLAYBACK_NAME

/usr/bin/volctrl.sh

AudioRoute
if [ -f /tmp/boot_complete ]; then
	StartSession
fi

exit 0
