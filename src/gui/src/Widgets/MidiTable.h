/*
 * Hydrogen
 * Copyright(c) 2002-2008 by Alex >Comix< Cominu [comix@users.sourceforge.net]
 * Copyright(c) 2008-2021 The hydrogen development team [hydrogen-devel@lists.sourceforge.net]
 *
 * http://www.hydrogen-music.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY, without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see https://www.gnu.org/licenses
 *
 */

#ifndef MIDI_TABLE_H
#define MIDI_TABLE_H

#include <core/Object.h>

#include <QtGui>
#include <QtWidgets>

class MidiTable : public QTableWidget, public H2Core::Object
{
    H2_OBJECT
	Q_OBJECT
	public:
		explicit MidiTable( QWidget* pParent );
		~MidiTable();

		void setupMidiTable();
		void saveMidiTable();
		void insertNewRow(QString, QString, int, int);

	private slots:
		void updateTable();
		void midiSensePressed( int );
	
	private:
		int __row_count;
		int currentMidiAutosenseRow;
		QSignalMapper *signalMapper;
		QTimer* m_pUpdateTimer;

};

#endif
