// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "NewInputRecordingDlg.h"

#include "QtUtils.h"
#include "pcsx2/Config.h"
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QString>
#include <QtWidgets/QDialog>
#include <QtWidgets/qfiledialog.h>

NewInputRecordingDlg::NewInputRecordingDlg(QWidget* parent)
	: QDialog(parent)
{
	m_ui.setupUi(this);

	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	setFixedSize(geometry().width(), geometry().height());

	// Default State
	m_ui.m_recTypeWarning->hide();
	m_ui.m_dlgBtns->button(QDialogButtonBox::Ok)->setEnabled(false);
	m_ui.m_filePathInput->setDisabled(true);
	// A default path permits saving into a folder which does not exist yet.
	const QDir directory(QString::fromStdString(EmuFolders::InputRecordings));
	m_filePath = QDir::toNativeSeparators(directory.filePath(QStringLiteral("recording.p2m2")));
	for (unsigned int suffix = 2; QFileInfo::exists(m_filePath); suffix++)
		m_filePath = QDir::toNativeSeparators(directory.filePath(QStringLiteral("recording (%1).p2m2").arg(suffix)));
	m_ui.m_filePathInput->setText(m_filePath);

	connect(m_ui.m_recTypePowerOn, &QRadioButton::clicked, this, &NewInputRecordingDlg::onRecordingTypePowerOnChecked);
	connect(m_ui.m_recTypeSaveState, &QRadioButton::clicked, this, &NewInputRecordingDlg::onRecordingTypeSaveStateChecked);

	connect(m_ui.m_filePathBrowseBtn, &QPushButton::clicked, this, &NewInputRecordingDlg::onBrowseForPathClicked);
	connect(m_ui.m_authorInput, &QLineEdit::textEdited, this, &NewInputRecordingDlg::onAuthorNameChanged);
}

NewInputRecordingDlg::~NewInputRecordingDlg() = default;

InputRecording::Type NewInputRecordingDlg::getInputRecType()
{
	return m_recType;
}

std::string NewInputRecordingDlg::getFilePath()
{
	return m_filePath.toStdString();
}

std::string NewInputRecordingDlg::getAuthorName()
{
	return m_authorName.toStdString();
}

void NewInputRecordingDlg::onRecordingTypePowerOnChecked(bool checked)
{
	if (checked)
	{
		m_recType = InputRecording::Type::POWER_ON;
		m_ui.m_recTypeWarning->hide();
	}
}

void NewInputRecordingDlg::onRecordingTypeSaveStateChecked(bool checked)
{
	if (checked)
	{
		m_recType = InputRecording::Type::FROM_SAVESTATE;
		m_ui.m_recTypeWarning->show();
	}
}

void NewInputRecordingDlg::onBrowseForPathClicked()
{
	QString filter = tr("Input Recording Files (*.p2m2)");
	QString filename = QDir::toNativeSeparators(QFileDialog::getSaveFileName(
		this, tr("Select a File"), m_filePath, filter, &filter));
	if (filename.isEmpty())
		return;

	m_filePath = std::move(filename);
	m_ui.m_filePathInput->setText(m_filePath);
	updateFormStatus();
}

void NewInputRecordingDlg::onAuthorNameChanged(const QString& text)
{
	m_authorName = text;
	updateFormStatus();
}

bool NewInputRecordingDlg::isFormValid()
{
	return !m_filePath.isEmpty() && !m_authorName.isEmpty();
}

void NewInputRecordingDlg::updateFormStatus()
{
	m_ui.m_dlgBtns->button(QDialogButtonBox::Ok)->setEnabled(isFormValid());
}

#include "moc_NewInputRecordingDlg.cpp"
