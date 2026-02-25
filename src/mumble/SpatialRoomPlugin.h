// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_SPATIALROOMPLUGIN_H_
#define MUMBLE_MUMBLE_SPATIALROOMPLUGIN_H_

#include <QtCore/QtGlobal>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QRadioButton>

#include <QtWidgets/QCheckBox>

#include "LegacyPlugin.h"

#ifndef USE_MANUAL_PLUGIN
#	include <QtCore/QHash>
struct Position2D {
	float x;
	float y;
};
using PositionMap = QHash< unsigned int, Position2D >;
Q_DECLARE_METATYPE(PositionMap)
#else
#	include "ManualPlugin.h"
#endif

#include <chrono>

constexpr int kSeatCount     = 8;
constexpr float kTableRadius = 1.5f;

struct StaleSpeaker {
	std::chrono::time_point< std::chrono::steady_clock > staleSince;
	int seat;
};

class SpatialRoom : public QDialog {
	Q_OBJECT
public:
	SpatialRoom(QWidget *parent = nullptr);

	static void setSpeakerPositions(const QHash< unsigned int, Position2D > &positions);
	void setActivated(bool active);

public slots:
	void on_seatClicked(int seat);
	void on_activatedToggled(bool checked);
	void on_speakerPositionUpdate(PositionMap positions);
	void on_updateStaleSpeakers();

protected:
	QRadioButton *m_seatButtons[kSeatCount];
	QLabel *m_seatStatusLabels[kSeatCount];
	QCheckBox *qcbActivated;
	QLabel *qlSeatInfo;

	QHash< unsigned int, int > m_speakerSeats;
	QHash< unsigned int, StaleSpeaker > m_staleSpeakers;
	QTimer *m_staleTimer;

	void changeEvent(QEvent *e);
	void updateSeatStatus();
	static int nearestSeat(float worldX, float worldZ);
};

MumblePlugin *SpatialRoomPlugin_getMumblePlugin();
MumblePluginQt *SpatialRoomPlugin_getMumblePluginQt();

class SpatialRoomPlugin : public LegacyPlugin {
	friend class Plugin;

private:
	Q_OBJECT
	Q_DISABLE_COPY(SpatialRoomPlugin)

protected:
	virtual void resolveFunctionPointers() Q_DECL_OVERRIDE;
	SpatialRoomPlugin(QObject *p = nullptr);

public:
	virtual ~SpatialRoomPlugin() Q_DECL_OVERRIDE;
};

#endif
