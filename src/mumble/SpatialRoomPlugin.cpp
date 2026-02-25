// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include <QtCore/QtCore>
#include <QtGui/QtGui>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QVBoxLayout>

#include "SpatialRoomPlugin.h"

#include <QPointer>
#include <QTimer>

#include <cmath>

#define MUMBLE_ALLOW_DEPRECATED_LEGACY_PLUGIN_API
#include "../../plugins/mumble_legacy_plugin.h"

#ifndef M_PI
#	define M_PI 3.14159265358979323846
#endif

// Precomputed seat positions and orientations
static struct SeatInfo {
	float pos[3];   // x, y, z
	float front[3]; // facing center
	float top[3];   // always (0, 1, 0)
} seats[kSeatCount];

static void initSeats() {
	for (int i = 0; i < kSeatCount; ++i) {
		float angle      = static_cast< float >(i) * static_cast< float >(M_PI) / 4.0f;
		seats[i].pos[0]  = kTableRadius * sinf(angle);
		seats[i].pos[1]  = 0.0f;
		seats[i].pos[2]  = kTableRadius * cosf(angle);

		seats[i].front[0] = -seats[i].pos[0] / kTableRadius;
		seats[i].front[1] = 0.0f;
		seats[i].front[2] = -seats[i].pos[2] / kTableRadius;

		seats[i].top[0] = 0.0f;
		seats[i].top[1] = 1.0f;
		seats[i].top[2] = 0.0f;
	}
}

// Static state
static QPointer< SpatialRoom > srDlg = nullptr;
static bool bLinked                   = false;

static struct {
	float avatar_pos[3];
	float avatar_front[3];
	float avatar_top[3];
	float camera_pos[3];
	float camera_front[3];
	float camera_top[3];
	std::string context;
	std::wstring identity;
	int currentSeat;
	bool seatsInitialized;
} my = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
		 std::string(), std::wstring(), 0, false };

static void applySeat(int seat) {
	if (!my.seatsInitialized) {
		initSeats();
		my.seatsInitialized = true;
	}
	my.currentSeat = seat;
	memcpy(my.avatar_pos, seats[seat].pos, sizeof(float) * 3);
	memcpy(my.avatar_front, seats[seat].front, sizeof(float) * 3);
	memcpy(my.avatar_top, seats[seat].top, sizeof(float) * 3);
	memcpy(my.camera_pos, seats[seat].pos, sizeof(float) * 3);
	memcpy(my.camera_front, seats[seat].front, sizeof(float) * 3);
	memcpy(my.camera_top, seats[seat].top, sizeof(float) * 3);
	my.context = "SpatialRoom";
}

// Grid positions for 8 seats arranged in a circle pattern on a 5x5 grid:
//
//          [0]
//       [7]   [1]
//     [6] (tbl) [2]
//       [5]   [3]
//          [4]
//
// Grid coords (row, col) in a 5-row x 5-col grid:
static const int seatGridRow[kSeatCount] = { 0, 0, 2, 3, 4, 3, 2, 0 };
static const int seatGridCol[kSeatCount] = { 2, 3, 4, 3, 2, 1, 0, 1 };

SpatialRoom::SpatialRoom(QWidget *p) : QDialog(p) {
	if (!my.seatsInitialized) {
		initSeats();
		my.seatsInitialized = true;
	}

	setWindowTitle(tr("Spatial Room"));

	auto *mainLayout = new QVBoxLayout(this);

	// Seat grid
	auto *seatGroup  = new QGroupBox(tr("Seats"), this);
	auto *gridLayout = new QGridLayout(seatGroup);
	gridLayout->setSpacing(4);

	auto *buttonGroup = new QButtonGroup(this);

	for (int i = 0; i < kSeatCount; ++i) {
		auto *container = new QVBoxLayout();
		container->setAlignment(Qt::AlignCenter);

		m_seatButtons[i] = new QRadioButton(tr("Seat %1").arg(i), seatGroup);
		m_seatButtons[i]->setChecked(i == my.currentSeat);
		buttonGroup->addButton(m_seatButtons[i], i);

		m_seatStatusLabels[i] = new QLabel(seatGroup);
		m_seatStatusLabels[i]->setAlignment(Qt::AlignCenter);
		m_seatStatusLabels[i]->setFixedHeight(16);

		container->addWidget(m_seatButtons[i], 0, Qt::AlignCenter);
		container->addWidget(m_seatStatusLabels[i], 0, Qt::AlignCenter);

		gridLayout->addLayout(container, seatGridRow[i], seatGridCol[i], Qt::AlignCenter);
	}

	// Table label in center
	auto *tableLabel = new QLabel(tr("Table"), seatGroup);
	tableLabel->setAlignment(Qt::AlignCenter);
	tableLabel->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
	gridLayout->addWidget(tableLabel, 2, 2, Qt::AlignCenter);

	mainLayout->addWidget(seatGroup);

	// Bottom bar: activated checkbox + seat info
	auto *bottomLayout = new QHBoxLayout();
	qcbActivated       = new QCheckBox(tr("Activated"), this);
	qcbActivated->setChecked(bLinked);
	qlSeatInfo = new QLabel(tr("Seat %1").arg(my.currentSeat), this);
	qlSeatInfo->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	bottomLayout->addWidget(qcbActivated);
	bottomLayout->addWidget(qlSeatInfo);
	mainLayout->addLayout(bottomLayout);

	connect(buttonGroup, QOverload< int >::of(&QButtonGroup::idClicked), this, &SpatialRoom::on_seatClicked);
	connect(qcbActivated, &QCheckBox::toggled, this, &SpatialRoom::on_activatedToggled);

	m_staleTimer = new QTimer(this);
	m_staleTimer->setInterval(100);
	connect(m_staleTimer, &QTimer::timeout, this, &SpatialRoom::on_updateStaleSpeakers);

	updateSeatStatus();
}

void SpatialRoom::setActivated(bool active) {
	qcbActivated->setChecked(active);
}

void SpatialRoom::setSpeakerPositions(const QHash< unsigned int, Position2D > &positions) {
	if (srDlg) {
		QMetaObject::invokeMethod(srDlg, "on_speakerPositionUpdate", Qt::QueuedConnection,
								  Q_ARG(PositionMap, positions));
	}
}

int SpatialRoom::nearestSeat(float worldX, float worldZ) {
	int best      = 0;
	float bestDst = 1e9f;
	for (int i = 0; i < kSeatCount; ++i) {
		float dx = worldX - seats[i].pos[0];
		float dz = worldZ - seats[i].pos[2];
		float d  = dx * dx + dz * dz;
		if (d < bestDst) {
			bestDst = d;
			best    = i;
		}
	}
	return best;
}

void SpatialRoom::on_seatClicked(int seat) {
	applySeat(seat);
	qlSeatInfo->setText(tr("Seat %1").arg(seat));
}

void SpatialRoom::on_activatedToggled(bool checked) {
	bLinked = checked;
	if (checked) {
		applySeat(my.currentSeat);
	}
}

void SpatialRoom::changeEvent(QEvent *e) {
	QDialog::changeEvent(e);
}

void SpatialRoom::updateSeatStatus() {
	// Build set of occupied seats (by other speakers)
	QSet< int > speakingSeats;
	QSet< int > staleSeats;

	for (auto it = m_speakerSeats.constBegin(); it != m_speakerSeats.constEnd(); ++it) {
		speakingSeats.insert(it.value());
	}
	for (auto it = m_staleSpeakers.constBegin(); it != m_staleSpeakers.constEnd(); ++it) {
		staleSeats.insert(it.value().seat);
	}

	for (int i = 0; i < kSeatCount; ++i) {
		if (speakingSeats.contains(i)) {
			m_seatStatusLabels[i]->setText(QStringLiteral("\xe2\x97\x89")); // filled circle (speaking)
			m_seatStatusLabels[i]->setStyleSheet(QStringLiteral("color: #e05050; font-size: 14px;"));
		} else if (staleSeats.contains(i)) {
			m_seatStatusLabels[i]->setText(QStringLiteral("\xe2\x97\x8b")); // empty circle (silent)
			m_seatStatusLabels[i]->setStyleSheet(QStringLiteral("color: #5080d0; font-size: 14px;"));
		} else {
			m_seatStatusLabels[i]->setText(QString());
			m_seatStatusLabels[i]->setStyleSheet(QString());
		}
	}
}

void SpatialRoom::on_speakerPositionUpdate(QHash< unsigned int, Position2D > positions) {
	// Restore any stale speakers that reappeared
	QMutableHashIterator< unsigned int, StaleSpeaker > staleIt(m_staleSpeakers);
	while (staleIt.hasNext()) {
		staleIt.next();
		if (positions.contains(staleIt.key())) {
			staleIt.remove();
		}
	}

	// Update existing or mark stale
	QMutableHashIterator< unsigned int, int > speakerIt(m_speakerSeats);
	while (speakerIt.hasNext()) {
		speakerIt.next();
		const unsigned int sessionID = speakerIt.key();

		if (positions.contains(sessionID)) {
			Position2D pos = positions.take(sessionID);
			speakerIt.value() = nearestSeat(pos.x, pos.y);
		} else {
			m_staleSpeakers.insert(sessionID, { std::chrono::steady_clock::now(), speakerIt.value() });
			speakerIt.remove();
		}
	}

	// Add new speakers
	QHashIterator< unsigned int, Position2D > newIt(positions);
	while (newIt.hasNext()) {
		newIt.next();
		Position2D pos = newIt.value();
		m_speakerSeats.insert(newIt.key(), nearestSeat(pos.x, pos.y));
	}

	if (!m_staleSpeakers.isEmpty() && !m_staleTimer->isActive()) {
		m_staleTimer->start();
	}

	updateSeatStatus();
}

void SpatialRoom::on_updateStaleSpeakers() {
	static constexpr double kStaleDisplayTime = 5.0;

	QMutableHashIterator< unsigned int, StaleSpeaker > staleIt(m_staleSpeakers);
	while (staleIt.hasNext()) {
		staleIt.next();
		double elapsed =
			static_cast< std::chrono::duration< double > >(std::chrono::steady_clock::now() - staleIt.value().staleSince)
				.count();
		if (elapsed >= kStaleDisplayTime) {
			staleIt.remove();
		}
	}

	if (m_staleSpeakers.isEmpty()) {
		m_staleTimer->stop();
	}

	updateSeatStatus();
}

// --- Legacy plugin API functions ---

static int trylock() {
	return bLinked;
}

static void unlock() {
	if (srDlg) {
		srDlg->setActivated(false);
	}
	bLinked = false;
}

static void config(void *ptr) {
	QWidget *w = reinterpret_cast< QWidget * >(ptr);

	if (srDlg) {
		srDlg->setParent(w, Qt::Dialog);
	} else {
		srDlg = new SpatialRoom(w);
	}

	srDlg->show();
}

static int fetch(float *avatar_pos, float *avatar_front, float *avatar_top, float *camera_pos, float *camera_front,
				 float *camera_top, std::string &context, std::wstring &identity) {
	if (!bLinked)
		return false;

	memcpy(avatar_pos, my.avatar_pos, sizeof(float) * 3);
	memcpy(avatar_front, my.avatar_front, sizeof(float) * 3);
	memcpy(avatar_top, my.avatar_top, sizeof(float) * 3);

	memcpy(camera_pos, my.camera_pos, sizeof(float) * 3);
	memcpy(camera_front, my.camera_front, sizeof(float) * 3);
	memcpy(camera_top, my.camera_top, sizeof(float) * 3);

	context.assign(my.context);
	identity.assign(my.identity);

	return true;
}

static const std::wstring longdesc() {
	return std::wstring(L"Spatial Room: 8-seat virtual meeting room for positional/HRTF audio testing.");
}

static std::wstring description(L"Spatial Room");
static std::wstring shortname(L"Spatial Room");

static void about(void *ptr) {
	QWidget *w = reinterpret_cast< QWidget * >(ptr);
	QMessageBox::about(w, QString::fromStdWString(description), QString::fromStdWString(longdesc()));
}

static MumblePlugin spatialroom = { MUMBLE_PLUGIN_MAGIC,
									description,
									shortname,
									nullptr, // About handled by MumblePluginQt
									nullptr, // Config handled by MumblePluginQt
									trylock,
									unlock,
									longdesc,
									fetch };

static MumblePluginQt spatialroomqt = { MUMBLE_PLUGIN_MAGIC_QT, about, config };

MumblePlugin *SpatialRoomPlugin_getMumblePlugin() {
	return &spatialroom;
}

MumblePluginQt *SpatialRoomPlugin_getMumblePluginQt() {
	return &spatialroomqt;
}

// --- SpatialRoomPlugin class (LegacyPlugin subclass) ---

SpatialRoomPlugin::SpatialRoomPlugin(QObject *p) : LegacyPlugin(QString::fromLatin1("spatialroom.builtin"), true, p) {
}

SpatialRoomPlugin::~SpatialRoomPlugin() {
}

void SpatialRoomPlugin::resolveFunctionPointers() {
	m_mumPlug   = &spatialroom;
	m_mumPlugQt = &spatialroomqt;
}
