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

#include "Version.h"

#include <cassert>
#include <memory>

#include <core/LocalFileMng.h>
#include <core/Preferences/Preferences.h>
#include <core/EventQueue.h>
#include <core/FX/Effects.h>
#include <core/Globals.h>
#include <core/Timeline.h>
#include <core/Basics/Song.h>
#include <core/Basics/DrumkitComponent.h>
#include <core/Basics/Sample.h>
#include <core/Basics/Instrument.h>
#include <core/Basics/InstrumentComponent.h>
#include <core/Basics/InstrumentList.h>
#include <core/Basics/InstrumentLayer.h>
#include <core/Basics/Pattern.h>
#include <core/Basics/PatternList.h>
#include <core/Basics/Note.h>
#include <core/Basics/AutomationPath.h>
#include <core/AutomationPathSerializer.h>
#include <core/Helpers/Xml.h>
#include <core/Helpers/Filesystem.h>
#include <core/Hydrogen.h>
#include <core/Sampler/Sampler.h>

#ifdef H2CORE_HAVE_OSC
#include <core/NsmClient.h>
#endif

#include <QDomDocument>
#include <QDir>

namespace
{

}//anonymous namespace
namespace H2Core
{

Song::Song( const QString& sName, const QString& sAuthor, float fBpm, float fVolume )
	: m_bIsTimelineActivated( false )
	, m_bIsMuted( false )
	, m_resolution( 48 )
	, m_fBpm( fBpm )
	, m_sName( sName )
	, m_sAuthor( sAuthor )
	, m_fVolume( fVolume )
	, m_fMetronomeVolume( 0.5 )
	, m_sNotes( "" )
	, m_pPatternList( nullptr )
	, m_pPatternGroupSequence( nullptr )
	, m_pInstrumentList( nullptr )
	, m_pComponents( nullptr )
	, m_sFilename( "" )
	, m_loopMode( LoopMode::Disabled )
	, m_patternMode( PatternMode::Selected )
	, m_fHumanizeTimeValue( 0.0 )
	, m_fHumanizeVelocityValue( 0.0 )
	, m_fSwingFactor( 0.0 )
	, m_bIsModified( false )
	, m_mode( Mode::Pattern )
	, m_sPlaybackTrackFilename( "" )
	, m_bPlaybackTrackEnabled( false )
	, m_fPlaybackTrackVolume( 0.0 )
	, m_pVelocityAutomationPath( nullptr )
	, m_license( License() )
	, m_actionMode( ActionMode::selectMode )
	, m_bIsPatternEditorLocked( false )
	, m_nPanLawType ( Sampler::RATIO_STRAIGHT_POLYGONAL )
	, m_fPanLawKNorm ( Sampler::K_NORM_DEFAULT )
	, m_currentDrumkitLookup( Filesystem::Lookup::stacked )
{
	INFOLOG( QString( "INIT '%1'" ).arg( sName ) );

	m_pComponents = new std::vector<DrumkitComponent*> ();
	m_pVelocityAutomationPath = new AutomationPath(0.0f, 1.5f,  1.0f);

	m_pTimeline = std::make_shared<Timeline>();
}

Song::~Song()
{
	/*
	 * Warning: it is not safe to delete a song without having a lock on the audio engine.
	 * Following the current design, the caller has to care for the lock.
	 */
	
	delete m_pPatternList;

	for (std::vector<DrumkitComponent*>::iterator it = m_pComponents->begin() ; it != m_pComponents->end(); ++it) {
		delete *it;
	}
	delete m_pComponents;

	if ( m_pPatternGroupSequence ) {
		for ( unsigned i = 0; i < m_pPatternGroupSequence->size(); ++i ) {
			PatternList* pPatternList = ( *m_pPatternGroupSequence )[i];
			pPatternList->clear();	// pulisco tutto, i pattern non vanno distrutti qua
			delete pPatternList;
		}
		delete m_pPatternGroupSequence;
	}

	delete m_pInstrumentList;

	delete m_pVelocityAutomationPath;

	INFOLOG( QString( "DESTROY '%1'" ).arg( m_sName ) );
}

void Song::setBpm( float fBpm ) {
	if ( fBpm > MAX_BPM ) {
		m_fBpm = MAX_BPM;
		WARNINGLOG( QString( "Provided bpm %1 is too high. Assigning upper bound %2 instead" )
					.arg( fBpm ).arg( MAX_BPM ) );
	} else if ( fBpm < MIN_BPM ) {
		m_fBpm = MIN_BPM;
		WARNINGLOG( QString( "Provided bpm %1 is too low. Assigning lower bound %2 instead" )
					.arg( fBpm ).arg( MIN_BPM ) );
	} else {
		m_fBpm = fBpm;
	}
}

void Song::setActionMode( Song::ActionMode actionMode ) {
	m_actionMode = actionMode;
}

long Song::lengthInTicks() const {
	long nSongLength = 0;
	int nColumns = m_pPatternGroupSequence->size();
	// Sum the lengths of all pattern columns and use the macro
	// MAX_NOTES in case some of them are of size zero.
	for ( int i = 0; i < nColumns; i++ ) {
		PatternList *pColumn = ( *m_pPatternGroupSequence )[ i ];
		if ( pColumn->size() != 0 ) {
			nSongLength += pColumn->longest_pattern_length();
		} else {
			nSongLength += MAX_NOTES;
		}
	}
    return nSongLength;
}

bool Song::isPatternActive( int nColumn, int nRow ) const {
	if ( nRow < 0 || nRow > m_pPatternList->size() ) {
		return false;
	}
	
	auto pPattern = m_pPatternList->get( nRow );
	if ( pPattern == nullptr ) {
		return false;
	}
	if ( nColumn < 0 || nColumn >= m_pPatternGroupSequence->size() ) {
		return false;
	}
	auto pColumn = ( *m_pPatternGroupSequence )[ nColumn ];
	if ( pColumn->index( pPattern ) == -1 ) {
		return false;
	}

	return true;
}
	
///Load a song from file
std::shared_ptr<Song> Song::load( const QString& sFilename )
{
	SongReader reader;
	return reader.readSong( sFilename );
}

/// Save a song to file
bool Song::save( const QString& sFilename )
{
	SongWriter writer;
	int err;
	err = writer.writeSong( shared_from_this(), sFilename );

	if( err ) {
		return false;
	}
	return QFile::exists( sFilename );
}

std::shared_ptr<Song> Song::getEmptySong()
{
	std::shared_ptr<Song> pSong =
		std::make_shared<Song>( Filesystem::untitled_song_name(), "hydrogen",
								120, 0.5 );

	pSong->setMetronomeVolume( 0.5 );
	pSong->setNotes( "..." );
	pSong->setLicense( License() );
	pSong->setLoopMode( Song::LoopMode::Disabled );
	pSong->setMode( Song::Mode::Pattern );
	pSong->setHumanizeTimeValue( 0.0 );
	pSong->setHumanizeVelocityValue( 0.0 );
	pSong->setSwingFactor( 0.0 );

	InstrumentList* pInstrList = new InstrumentList();
	auto pNewInstr = std::make_shared<Instrument>( EMPTY_INSTR_ID, "New instrument" );
	pInstrList->add( pNewInstr );
	pSong->setInstrumentList( pInstrList );

	PatternList*	pPatternList = new PatternList();
	PatternList*    patternSequence = new PatternList();

	for ( int nn = 0; nn < 10; ++nn ) {
		Pattern*		pEmptyPattern = new Pattern();
	
		pEmptyPattern->set_name( QString( "Pattern %1" ).arg( nn + 1 ) );
		pEmptyPattern->set_category( QString( "not_categorized" ) );
		pPatternList->add( pEmptyPattern );

		if ( nn == 0 ) {
			// Only the first pattern will be activated in the
			// SongEditor.
			patternSequence->add( pEmptyPattern );
		}
	}
	pSong->setPatternList( pPatternList );
	
	std::vector<PatternList*>* pPatternGroupVector = new std::vector<PatternList*>;
	pPatternGroupVector->push_back( patternSequence );
	pSong->setPatternGroupVector( pPatternGroupVector );

	pSong->setFilename( Filesystem::empty_song_path() );

	auto pDrumkit = H2Core::Drumkit::load_by_name( Filesystem::drumkit_default_kit(), true );
	if ( pDrumkit == nullptr ) {
		ERRORLOG( QString( "Unabled to load default Drumkit [%1]" )
				  .arg( Filesystem::drumkit_default_kit() ) );
	} else {
		pSong->loadDrumkit( pDrumkit, true );
		delete pDrumkit;
	}
	
	pSong->setIsModified( false );

	return pSong;

}

DrumkitComponent* Song::getComponent( int nID ) const
{
	for (std::vector<DrumkitComponent*>::iterator it = m_pComponents->begin() ; it != m_pComponents->end(); ++it) {
		if( (*it)->get_id() == nID ) {
			return *it;
		}
	}

	return nullptr;
}


void Song::setSwingFactor( float factor )
{
	if ( factor < 0.0 ) {
		factor = 0.0;
	} else if ( factor > 1.0 ) {
		factor = 1.0;
	}

	m_fSwingFactor = factor;
}

void Song::setIsModified( bool bIsModified )
{
	bool Notify = false;

	if( m_bIsModified != bIsModified ) {
		Notify = true;
	}

	m_bIsModified = bIsModified;

	if( Notify ) {
		EventQueue::get_instance()->push_event( EVENT_SONG_MODIFIED, -1 );

		if ( Hydrogen::get_instance()->isUnderSessionManagement() ) {
			// If Hydrogen is under session management (NSM), tell the
			// NSM server that the Song was modified.
#ifdef H2CORE_HAVE_OSC
			NsmClient::get_instance()->sendDirtyState( bIsModified );
#endif
		}
	}
	
}

bool Song::hasMissingSamples() const
{
	InstrumentList *pInstrumentList = getInstrumentList();
	for ( int i = 0; i < pInstrumentList->size(); i++ ) {
		if ( pInstrumentList->get( i )->has_missing_samples() ) {
			return true;
		}
	}
	return false;
}

void Song::clearMissingSamples() {
	InstrumentList *pInstrumentList = getInstrumentList();
	for ( int i = 0; i < pInstrumentList->size(); i++ ) {
		pInstrumentList->get( i )->set_missing_samples( false );
	}
}

void Song::readTempPatternList( const QString& sFilename )
{
	XMLDoc doc;
	if( !doc.read( sFilename ) ) {
		return;
	}
	XMLNode root = doc.firstChildElement( "sequence" );
	if ( root.isNull() ) {
		ERRORLOG( "sequence node not found" );
		return;
	}

	XMLNode virtualsNode = root.firstChildElement( "virtuals" );
	if ( !virtualsNode.isNull() ) {
		XMLNode virtualNode = virtualsNode.firstChildElement( "virtual" );
		while ( !virtualNode.isNull() ) {
			QString patternName = virtualNode.read_attribute( "pattern", nullptr, false, false );
			XMLNode patternNode = virtualNode.firstChildElement( "pattern" );
			Pattern* pPattern = nullptr;
			while ( !patternName.isEmpty() && !patternNode.isNull() ) {
				QString virtualName = patternNode.read_text( false );
				if ( !virtualName.isEmpty() ) {
					Pattern* pVirtualPattern = nullptr;
					for ( unsigned i = 0; i < getPatternList()->size(); i++ ) {
						Pattern* pat = getPatternList()->get( i );
						if ( pPattern == nullptr && pat->get_name() == patternName ) {
							pPattern = pat;
						}
						if ( pVirtualPattern == nullptr && pat->get_name() == virtualName ) {
							pVirtualPattern = pat;
						}
						if ( pPattern != nullptr && pVirtualPattern != nullptr) {
							break;
						}
					}
					if ( pPattern == nullptr ) {
						ERRORLOG( QString( "Invalid pattern name %1" ).arg( patternName ) );
					}
					if ( pVirtualPattern == nullptr ) {
						ERRORLOG( QString( "Invalid virtual pattern name %1" ).arg( virtualName ) );
					}
					if ( pPattern != nullptr && pVirtualPattern != nullptr ) {
						pPattern->virtual_patterns_add( pVirtualPattern );
					}
				}
				patternNode = patternNode.nextSiblingElement( "pattern" );
			}
			virtualNode = virtualNode.nextSiblingElement( "virtual" );
		}
	} else {
		WARNINGLOG( "no virtuals node not found" );
	}

	getPatternList()->flattened_virtual_patterns_compute();
	getPatternGroupVector()->clear();

	XMLNode sequenceNode = root.firstChildElement( "groups" );
	if ( !sequenceNode.isNull() ) {
		XMLNode groupNode = sequenceNode.firstChildElement( "group" );
		while ( !groupNode.isNull() ) {
			PatternList* patternSequence = new PatternList();
			XMLNode patternNode = groupNode.firstChildElement( "pattern" );
			while ( !patternNode.isNull() ) {
				QString patternName = patternNode.read_text( false );
				if( !patternName.isEmpty() ) {
					Pattern* p = nullptr;
					for ( unsigned i = 0; i < getPatternList()->size(); i++ ) {
						Pattern* pat = getPatternList()->get( i );
						if ( pat->get_name() == patternName ) {
							p = pat;
							break;
						}
					}
					if ( p == nullptr ) {
						ERRORLOG( QString( "Invalid pattern name %1" ).arg( patternName ) );
					} else {
						patternSequence->add( p );
					}
				}
				patternNode = patternNode.nextSiblingElement( "pattern" );
			}
			getPatternGroupVector()->push_back( patternSequence );
			groupNode = groupNode.nextSiblingElement( "group" );
		}
	} else {
		WARNINGLOG( "no sequence node not found" );
	}
}

bool Song::writeTempPatternList( const QString& sFilename )
{
	XMLDoc doc;
	XMLNode root = doc.set_root( "sequence" );

	XMLNode virtualPatternListNode = root.createNode( "virtuals" );
	for ( unsigned i = 0; i < getPatternList()->size(); i++ ) {
		Pattern *pPattern = getPatternList()->get( i );
		if ( !pPattern->get_virtual_patterns()->empty() ) {
			XMLNode node = virtualPatternListNode.createNode( "virtual" );
			node.write_attribute( "pattern", pPattern->get_name() );
			for ( Pattern::virtual_patterns_it_t virtIter = pPattern->get_virtual_patterns()->begin(); virtIter != pPattern->get_virtual_patterns()->end(); ++virtIter ) {
				node.write_string( "pattern", (*virtIter)->get_name() );
			}
		}
	}

	XMLNode patternSequenceNode = root.createNode( "groups" );
	for ( unsigned i = 0; i < getPatternGroupVector()->size(); i++ ) {
		XMLNode node = patternSequenceNode.createNode( "group" );
		PatternList *pList = ( *getPatternGroupVector() )[i];
		for ( unsigned j = 0; j < pList->size(); j++ ) {
			Pattern *pPattern = pList->get( j );
			node.write_string( "pattern", pPattern->get_name() );
		}
	}

	return doc.write( sFilename );
}

QString Song::copyInstrumentLineToString( int nSelectedPattern, int nSelectedInstrument )
{
	auto pInstr = getInstrumentList()->get( nSelectedInstrument );
	assert( pInstr );

	QDomDocument doc;
	QDomProcessingInstruction header = doc.createProcessingInstruction( "xml", "version=\"1.0\" encoding=\"UTF-8\"");
	doc.appendChild( header );

	QDomNode rootNode = doc.createElement( "instrument_line" );
	//LIB_ID just in work to get better usability
	//LocalFileMng::writeXmlString( &rootNode, "LIB_ID", "in_work" );
	LocalFileMng::writeXmlString( rootNode, "author", getAuthor() );
	LocalFileMng::writeXmlString( rootNode, "license", getLicense().getLicenseString() );

	QDomNode patternList = doc.createElement( "patternList" );

	unsigned nPatterns = getPatternList()->size();
	for ( unsigned i = 0; i < nPatterns; i++ )
	{
		if (( nSelectedPattern >= 0 ) && ( nSelectedPattern != i ) ) {
			continue;
		}

		// Export pattern
		Pattern *pPattern = getPatternList()->get( i );

		QDomNode patternNode = doc.createElement( "pattern" );
		LocalFileMng::writeXmlString( patternNode, "pattern_name", pPattern->get_name() );

		QString category;
		if ( pPattern->get_category().isEmpty() ) {
			category = "No category";
		} else {
			category = pPattern->get_category();
		}

		LocalFileMng::writeXmlString( patternNode, "info", pPattern->get_info() );
		LocalFileMng::writeXmlString( patternNode, "category", category  );
		LocalFileMng::writeXmlString( patternNode, "size", QString("%1").arg( pPattern->get_length() ) );
		LocalFileMng::writeXmlString( patternNode, "denominator", QString("%1").arg( pPattern->get_denominator() ) );
		QDomNode noteListNode = doc.createElement( "noteList" );
		const Pattern::notes_t* notes = pPattern->get_notes();
		FOREACH_NOTE_CST_IT_BEGIN_END(notes,it)
		{
			Note *pNote = it->second;
			assert( pNote );

			// Export only specified instrument
			if (pNote->get_instrument() == pInstr)
			{
				XMLNode noteNode = doc.createElement( "note" );
				pNote->save_to( &noteNode );
				noteListNode.appendChild( noteNode );
			}
		}
		patternNode.appendChild( noteListNode );

		patternList.appendChild( patternNode );
	}

	rootNode.appendChild(patternList);

	doc.appendChild( rootNode );

	// Serialize document & return
	return doc.toString();
}

bool Song::pasteInstrumentLineFromString( const QString& sSerialized, int nSelectedPattern, int nSelectedInstrument, std::list<Pattern *>& pPatterns )
{
	QDomDocument doc;
	if ( !doc.setContent( sSerialized ) ) {
		return false;
	}

	// Get current instrument
	auto pInstr = getInstrumentList()->get( nSelectedInstrument );
	assert( pInstr );

	// Get pattern list
	PatternList *pList = getPatternList();
	Pattern *pSelected = ( nSelectedPattern >= 0 ) ? pList->get( nSelectedPattern ) : nullptr;
	QDomNode patternNode;
	bool bIsNoteSelection = false;
	bool is_single = true;

	// Check if document has correct structure
	QDomNode rootNode = doc.firstChildElement( "instrument_line" );	// root element
	if ( ! rootNode.isNull() ) {
		// Find pattern list
		QDomNode patternList = rootNode.firstChildElement( "patternList" );
		if ( patternList.isNull() ) {
			return false;
		}

		// Parse each pattern if needed
		patternNode = patternList.firstChildElement( "pattern" );
		if (!patternNode.isNull()) {
			is_single = (( QDomNode )patternNode.nextSiblingElement( "pattern" )).isNull();
		}

	} else {
		rootNode = doc.firstChildElement( "noteSelection" );
		if ( ! rootNode.isNull() ) {
			// Found a noteSelection. This contains a noteList, as a <pattern> does, so treat this as an anonymous pattern.
			bIsNoteSelection = true;
			is_single = true;
			patternNode = rootNode;

		} else {
			ERRORLOG( "Error pasting Clipboard:instrument_line or noteSelection node not found ");
			return false;
		}
	}

	while (!patternNode.isNull())
	{
		QString patternName(LocalFileMng::readXmlString(patternNode, "pattern_name", ""));

		// Check if pattern name specified
		if (patternName.length() > 0 || bIsNoteSelection )
		{
			// Try to find pattern by name
			Pattern* pat = pList->find(patternName);

			// If OK - check if need to add this pattern to result
			// If there is only one pattern, we always add it to list
			// If there is no selected pattern, we add all existing patterns to list (match by name)
			// Otherwise we add only existing selected pattern to list (match by name)
			if ((is_single) || ((pat != nullptr) && ((nSelectedPattern < 0) || (pat == pSelected))))
			{
				// Load additional pattern info & create pattern
				QString sInfo;
				sInfo = LocalFileMng::readXmlString(patternNode, "info", sInfo, false, false);
				QString sCategory;
				sCategory = LocalFileMng::readXmlString(patternNode, "category", sCategory, false, false);
				int nSize = -1;
				nSize = LocalFileMng::readXmlInt(patternNode, "size", nSize, false, false);

				// Change name of pattern to selected pattern
				if (pSelected != nullptr) {
					patternName = pSelected->get_name();
				}

				pat = new Pattern( patternName, sInfo, sCategory, nSize );

				// Parse pattern data
				QDomNode pNoteListNode = patternNode.firstChildElement( "noteList" );
				if ( ! pNoteListNode.isNull() )
				{
					// Parse note-by-note
					XMLNode noteNode = pNoteListNode.firstChildElement( "note" );
					while ( ! noteNode.isNull() )
					{
						QDomNode instrument = noteNode.firstChildElement( "instrument" );
						QDomNode instrumentText = instrument.firstChild();

						instrumentText.setNodeValue( QString::number( pInstr->get_id() ) );
						Note *pNote = Note::load_from( &noteNode, getInstrumentList() );

						pat->insert_note( pNote ); // Add note to created pattern

						noteNode = ( QDomNode ) noteNode.nextSiblingElement( "note" );
					}
				}

				// Add loaded pattern to apply-list
				pPatterns.push_back(pat);
			}
		}

		patternNode = ( QDomNode ) patternNode.nextSiblingElement( "pattern" );
	}

	return true;
}


void Song::setPanLawKNorm( float fKNorm ) {
	if ( fKNorm >= 0. ) {
		m_fPanLawKNorm = fKNorm;
	} else {
		WARNINGLOG("negative kNorm. Set default" );
		m_fPanLawKNorm = Sampler::K_NORM_DEFAULT;
	}
}

void Song::loadDrumkit( Drumkit *pDrumkit, bool bConditional ) {
	assert ( pDrumkit );
	auto pAudioEngine = Hydrogen::get_instance()->getAudioEngine();

	m_sCurrentDrumkitName = pDrumkit->get_name();
	if ( pDrumkit->isUserDrumkit() ) {
		m_currentDrumkitLookup = Filesystem::Lookup::user;
	} else {
		m_currentDrumkitLookup = Filesystem::Lookup::system;
	}


	// Load DrumkitComponents 
	std::vector<DrumkitComponent*>* pDrumkitCompoList = pDrumkit->get_components();
	
	for( auto &pComponent : *m_pComponents ){
		delete pComponent;
	}
	m_pComponents->clear();
	
	for (std::vector<DrumkitComponent*>::iterator it = pDrumkitCompoList->begin() ; it != pDrumkitCompoList->end(); ++it) {
		DrumkitComponent* pSrcComponent = *it;
		DrumkitComponent* pNewComponent = new DrumkitComponent( pSrcComponent->get_id(), pSrcComponent->get_name() );
		pNewComponent->load_from( pSrcComponent );

		m_pComponents->push_back( pNewComponent );
	}

	//////
	// Load InstrumentList
	/*
	 * If the old drumkit is bigger then the new drumkit,
	 * delete all instruments with a bigger pos then
	 * pDrumkitInstrList->size(). Otherwise the instruments
	 * from our old instrumentlist with
	 * pos > pDrumkitInstrList->size() stay in the
	 * new instrumentlist
	 */
	InstrumentList *pDrumkitInstrList = pDrumkit->get_instruments();
	
	int nInstrumentDiff = m_pInstrumentList->size() - pDrumkitInstrList->size();
	int nMaxID = -1;
	
	std::shared_ptr<Instrument> pInstr, pNewInstr;
	for ( int nnInstr = 0; nnInstr < pDrumkitInstrList->size(); ++nnInstr ) {
		if ( nnInstr < m_pInstrumentList->size() ) {
			// Instrument exists already
			pInstr = m_pInstrumentList->get( nnInstr );
			assert( pInstr );
		} else {
			pInstr = std::make_shared<Instrument>();
			m_pInstrumentList->add( pInstr );
		}

		pNewInstr = pDrumkitInstrList->get( nnInstr );
		assert( pNewInstr );
		INFOLOG( QString( "Loading instrument (%1 of %2) [%3]" )
				 .arg( nnInstr + 1 )
				 .arg( pDrumkitInstrList->size() )
				 .arg( pNewInstr->get_name() ) );

		// Preserve instrument IDs. Where the new drumkit has more
		// instruments than the song does, new instruments need new
		// ids.
		int nID = pInstr->get_id();
		if ( nID == EMPTY_INSTR_ID ) {
			nID = nMaxID + 1;
		}
		nMaxID = std::max( nID, nMaxID );

		pInstr->load_from( pDrumkit, pNewInstr );
		pInstr->set_id( nID );
	}

	// Discard redundant instruments (in case the last drumkit had
	// more instruments than the new one).
	if ( nInstrumentDiff >= 0 ) {
		for ( int i = 0; i < nInstrumentDiff ; i++ ){
			removeInstrument( m_pInstrumentList->size() - 1,
							  bConditional );
		}
	}
}

void Song::removeInstrument( int nInstrumentNumber, bool bConditional ) {
	auto pHydrogen = Hydrogen::get_instance();
	auto pInstr = m_pInstrumentList->get( nInstrumentNumber );
	if ( pInstr == nullptr ) {
		// Error log is already printed by get().
		return;
	}

	if ( bConditional ) {
		// If a note was assigned to this instrument in any pattern,
		// the instrument will be kept instead of discarded.
		for ( const auto& pPattern : *m_pPatternList ) {
			if ( pPattern->references( pInstr ) ) {
				DEBUGLOG("Keeping instrument #" + QString::number( nInstrumentNumber ) );
				return;
			}
		}
	} else {
		for ( const auto& pPattern : *m_pPatternList ) {
			pPattern->purge_instrument( pInstr, false );
		}
	}

	// In case there is just this one instrument left, reset it
	// instead of removing it.
	if ( m_pInstrumentList->size() == 1 ){
		pInstr->set_name( (QString( "Instrument 1" )) );
		for ( auto& pCompo : *pInstr->get_components() ) {
			// remove all layers
			for ( int nLayer = 0; nLayer < InstrumentComponent::getMaxLayers(); nLayer++ ) {
				pCompo->set_layer( nullptr, nLayer );
			}
		}
		DEBUGLOG("clear last instrument to empty instrument 1 instead delete the last instrument");
		return;
	}
	
	// delete the instrument from the instruments list
	m_pInstrumentList->del( nInstrumentNumber );

	// At this point the instrument has been removed from both the
	// instrument list and every pattern in the song.  Hence there's no way
	// (NOTE) to play on that instrument, and once all notes have stopped
	// playing it will be save to delete.
	// the ugly name is just for debugging...
	QString xxx_name = QString( "XXX_%1" ).arg( pInstr->get_name() );
	pInstr->set_name( xxx_name );
	pHydrogen->addInstrumentToDeathRow( pInstr );
}

std::vector<std::shared_ptr<Note>> Song::getAllNotes() const {

	std::vector<std::shared_ptr<Note>> notes;

	long nColumnStartTick = 0;
	for ( int ii = 0; ii < m_pPatternGroupSequence->size(); ++ii ) {

		auto pColumn = (*m_pPatternGroupSequence)[ ii ];
		
		if ( pColumn->size() == 0 ) {
			// An empty column with no patterns selected (but not the
			// end of the song).
			nColumnStartTick += MAX_NOTES;
			continue;
		} else {

			pColumn->longest_pattern_length();
			for ( const auto& ppattern : *pColumn ) {
				if ( ppattern != nullptr ) {
					FOREACH_NOTE_CST_IT_BEGIN_END( ppattern->get_notes(), it ) {
						if ( it->second != nullptr ) {
							// Use the copy constructor to not mess
							// with the song itself.
							std::shared_ptr<Note> pNote =
								std::make_shared<Note>( it->second );

							// The position property of the note
							// specifies its position within the
							// pattern. All we need to do is to add
							// the pattern start tick.
							pNote->set_position( pNote->get_position() +
												 nColumnStartTick );
							notes.push_back( pNote );
						}
					}
				}
			}

			nColumnStartTick += pColumn->longest_pattern_length();
		}
	}
	
	return notes;
}

int Song::findExistingComponent( const QString& sComponentName ) const {
	for ( const auto& ppComponent : *m_pComponents ) {
		if ( ppComponent->get_name().compare( sComponentName ) == 0 ){
			return ppComponent->get_id();
		}
	}
	return -1;
}

int Song::findFreeComponentID( int nStartingID ) const {

	bool bFreeID = true;
	
	for ( const auto& ppComponent : *m_pComponents ) {
		if ( ppComponent->get_id() == nStartingID ) {
			bFreeID = false;
			break;
		}
	}

	if ( bFreeID ) {
		return nStartingID;
	}
	else {
		return findFreeComponentID( nStartingID + 1 );
	}
}

QString Song::makeComponentNameUnique( const QString& sName ) const {
	for ( const auto& ppComponent : *m_pComponents ) {
		if ( ppComponent->get_name().compare( sName ) == 0 ){
			return makeComponentNameUnique( sName + "_new" );
		}
	}
	return sName;
}
 
QString Song::toQString( const QString& sPrefix, bool bShort ) const {
	QString s = Base::sPrintIndention;
	QString sOutput;
	if ( ! bShort ) {
		sOutput = QString( "%1[Song]\n" ).arg( sPrefix )
			.append( QString( "%1%2m_bIsTimelineActivated: %3\n" ).arg( sPrefix ).arg( s ).arg( m_bIsTimelineActivated ) )
			.append( QString( "%1%2m_bIsMuted: %3\n" ).arg( sPrefix ).arg( s ).arg( m_bIsMuted ) )
			.append( QString( "%1%2m_resolution: %3\n" ).arg( sPrefix ).arg( s ).arg( m_resolution ) )
			.append( QString( "%1%2m_fBpm: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fBpm ) )
			.append( QString( "%1%2m_sName: %3\n" ).arg( sPrefix ).arg( s ).arg( m_sName ) )
			.append( QString( "%1%2m_sAuthor: %3\n" ).arg( sPrefix ).arg( s ).arg( m_sAuthor ) )
			.append( QString( "%1%2m_fVolume: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fVolume ) )
			.append( QString( "%1%2m_fMetronomeVolume: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fMetronomeVolume ) )
			.append( QString( "%1%2m_sNotes: %3\n" ).arg( sPrefix ).arg( s ).arg( m_sNotes ) )
			.append( QString( "%1" ).arg( m_pPatternList->toQString( sPrefix + s, bShort ) ) )
			.append( QString( "%1%2m_pPatternGroupSequence:\n" ).arg( sPrefix ).arg( s ) );
		for ( auto pp : *m_pPatternGroupSequence ) {
			if ( pp != nullptr ) {
				sOutput.append( QString( "%1" ).arg( pp->toQString( sPrefix + s + s, bShort ) ) );
			}
		}
		sOutput.append( QString( "%1" ).arg( m_pInstrumentList->toQString( sPrefix + s, bShort ) ) )
			.append( QString( "%1%2m_pComponents:\n" ).arg( sPrefix ).arg( s ) );
		for ( auto cc : *m_pComponents ) {
			if ( cc != nullptr ) {
				sOutput.append( QString( "%1" ).arg( cc->toQString( sPrefix + s + s ) ) );
			}
		}
		sOutput.append( QString( "%1%2m_sFilename: %3\n" ).arg( sPrefix ).arg( s ).arg( m_sFilename ) )
			.append( QString( "%1%2m_loopMode: %3\n" ).arg( sPrefix ).arg( s ).arg( static_cast<int>(m_loopMode) ) )
			.append( QString( "%1%2m_fHumanizeTimeValue: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fHumanizeTimeValue ) )
			.append( QString( "%1%2m_fHumanizeVelocityValue: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fHumanizeVelocityValue ) )
			.append( QString( "%1%2m_fSwingFactor: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fSwingFactor ) )
			.append( QString( "%1%2m_bIsModified: %3\n" ).arg( sPrefix ).arg( s ).arg( m_bIsModified ) )
			.append( QString( "%1%2m_latestRoundRobins\n" ).arg( sPrefix ).arg( s ) );
		for ( auto mm : m_latestRoundRobins ) {
			sOutput.append( QString( "%1%2%3 : %4\n" ).arg( sPrefix ).arg( s ).arg( mm.first ).arg( mm.second ) );
		}
		sOutput.append( QString( "%1%2m_songMode: %3\n" ).arg( sPrefix ).arg( s )
						.arg( static_cast<int>(m_mode )) )
			.append( QString( "%1%2m_sPlaybackTrackFilename: %3\n" ).arg( sPrefix ).arg( s ).arg( m_sPlaybackTrackFilename ) )
			.append( QString( "%1%2m_bPlaybackTrackEnabled: %3\n" ).arg( sPrefix ).arg( s ).arg( m_bPlaybackTrackEnabled ) )
			.append( QString( "%1%2m_fPlaybackTrackVolume: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fPlaybackTrackVolume ) )
			.append( QString( "%1" ).arg( m_pVelocityAutomationPath->toQString( sPrefix + s, bShort ) ) )
			.append( QString( "%1%2m_license: %3\n" ).arg( sPrefix ).arg( s ).arg( m_license.toQString() ) )
			.append( QString( "%1%2m_actionMode: %3\n" ).arg( sPrefix ).arg( s )
					 .arg( static_cast<int>(m_actionMode) ) )
			.append( QString( "%1%2m_nPanLawType: %3\n" ).arg( sPrefix ).arg( s ).arg( m_nPanLawType ) )
			.append( QString( "%1%2m_fPanLawKNorm: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fPanLawKNorm ) )
			.append( QString( "%1%2m_pTimeline:\n" ).arg( sPrefix ).arg( s ) );
		if ( m_pTimeline != nullptr ) {
			sOutput.append( QString( "%1" ).arg( m_pTimeline->toQString( sPrefix + s, bShort ) ) );
		} else {
			sOutput.append( QString( "nullptr\n" ) );
		}
		sOutput.append( QString( "%1%2m_sCurrentDrumkitName: %3\n" ).arg( sPrefix ).arg( s ).arg( m_sCurrentDrumkitName ) )
			.append( QString( "%1%2m_currentDrumkitLookup: %3\n" ).arg( sPrefix ).arg( s ).arg( static_cast<int>(m_currentDrumkitLookup) ) );
	} else {
		
		sOutput = QString( "[Song]" )
			.append( QString( ", m_bIsTimelineActivated: %1" ).arg( m_bIsTimelineActivated ) )
			.append( QString( ", m_bIsMuted: %1" ).arg( m_bIsMuted ) )
			.append( QString( ", m_resolution: %1" ).arg( m_resolution ) )
			.append( QString( ", m_fBpm: %1" ).arg( m_fBpm ) )
			.append( QString( ", m_sName: %1" ).arg( m_sName ) )
			.append( QString( ", m_sAuthor: %1" ).arg( m_sAuthor ) )
			.append( QString( ", m_fVolume: %1" ).arg( m_fVolume ) )
			.append( QString( ", m_fMetronomeVolume: %1" ).arg( m_fMetronomeVolume ) )
			.append( QString( ", m_sNotes: %1" ).arg( m_sNotes ) )
			.append( QString( "%1" ).arg( m_pPatternList->toQString( sPrefix + s, bShort ) ) )
			.append( QString( ", m_pPatternGroupSequence:" ) );
		for ( auto pp : *m_pPatternGroupSequence ) {
			if ( pp != nullptr ) {
				sOutput.append( QString( "%1" ).arg( pp->toQString( sPrefix + s + s, bShort ) ) );
			}
		}
		sOutput.append( QString( "%1" ).arg( m_pInstrumentList->toQString( sPrefix + s, bShort ) ) )
			.append( QString( ", m_pComponents: [" ) );
		for ( auto cc : *m_pComponents ) {
			if ( cc != nullptr ) {
				sOutput.append( QString( "%1" ).arg( cc->toQString( sPrefix + s + s, bShort ) ) );
			}
		}
		sOutput.append( QString( "], m_sFilename: %1" ).arg( m_sFilename ) )
			.append( QString( ", m_loopMode: %1" ).arg( static_cast<int>(m_loopMode) ) )
			.append( QString( ", m_fHumanizeTimeValue: %1" ).arg( m_fHumanizeTimeValue ) )
			.append( QString( ", m_fHumanizeVelocityValue: %1" ).arg( m_fHumanizeVelocityValue ) )
			.append( QString( ", m_fSwingFactor: %1" ).arg( m_fSwingFactor ) )
			.append( QString( ", m_bIsModified: %1" ).arg( m_bIsModified ) )
			.append( QString( ", m_latestRoundRobins" ) );
		for ( auto mm : m_latestRoundRobins ) {
			sOutput.append( QString( ", %1 : %4" ).arg( mm.first ).arg( mm.second ) );
		}
		sOutput.append( QString( ", m_mode: %1" )
						.arg( static_cast<int>(m_mode) ) )
			.append( QString( ", m_sPlaybackTrackFilename: %1" ).arg( m_sPlaybackTrackFilename ) )
			.append( QString( ", m_bPlaybackTrackEnabled: %1" ).arg( m_bPlaybackTrackEnabled ) )
			.append( QString( ", m_fPlaybackTrackVolume: %1" ).arg( m_fPlaybackTrackVolume ) )
			.append( QString( ", m_pVelocityAutomationPath: %1" ).arg( m_pVelocityAutomationPath->toQString( sPrefix ) ) )
			.append( QString( ", m_license: %1" ).arg( m_license.toQString() ) )
			.append( QString( ", m_actionMode: %1" ).arg( static_cast<int>(m_actionMode) ) )
			.append( QString( ", m_nPanLawType: %1" ).arg( m_nPanLawType ) )
			.append( QString( ", m_fPanLawKNorm: %1" ).arg( m_fPanLawKNorm ) )
			.append( QString( ", m_pTimeline: " ) );
		if ( m_pTimeline != nullptr ) {
			sOutput.append( QString( "%1" ).arg( m_pTimeline->toQString( sPrefix, bShort ) ) );
		} else {
			sOutput.append( QString( "nullptr" ) );
		}
		sOutput.append( QString( ", m_sCurrentDrumkitName: %1" ).arg( m_sCurrentDrumkitName ) )
			.append( QString( ", m_currentDrumkitLookup: %1" ).arg( static_cast<int>(m_currentDrumkitLookup) ) );
			
	}
	
	return sOutput;
}

//-----------------------------------------------------------------------------
//	Implementation of SongReader class
//-----------------------------------------------------------------------------

SongReader::SongReader()
{
//	infoLog("init");
}

SongReader::~SongReader()
{
//	infoLog("destroy");
}


const QString SongReader::getPath ( const QString& sFilename ) const
{
	/* Try direct path */
	if ( QFile( sFilename ).exists() ) {
		return QFileInfo ( sFilename ).absoluteFilePath();
	}

	/* Try search in Session Directory */
	char* sesdir = getenv ( "SESSION_DIR" );
	if ( sesdir ) {
		INFOLOG ( "Try SessionDirectory " + QString( sesdir ) );
		QDir SesDir( sesdir );
		QString BaseFileName = QFileInfo( sFilename ).fileName();
		QString SesFileName = SesDir.filePath( BaseFileName );
		if ( QFile( SesFileName ).exists() ) {
			return QFileInfo( SesFileName ).absoluteFilePath();
		}
	}

	ERRORLOG( "Song file " + sFilename + " not found." );
	return nullptr;
}

///
/// Reads a song.
/// return nullptr = error reading song file.
///
std::shared_ptr<Song> SongReader::readSong( const QString& sFileName )
{
	QString sFilename = getPath( sFileName );
	if ( sFilename.isEmpty() ) {
		return nullptr;
	}

	auto pPreferences = Preferences::get_instance();

	INFOLOG( "Reading " + sFilename );
	std::shared_ptr<Song> pSong = nullptr;

	QDomDocument doc = LocalFileMng::openXmlDocument( sFilename );
	QDomNodeList nodeList = doc.elementsByTagName( "song" );

	if( nodeList.isEmpty() ) {
		ERRORLOG( "Error reading song: song node not found" );
		return nullptr;
	}

	QDomNode songNode = nodeList.at( 0 );

	m_sSongVersion = LocalFileMng::readXmlString( songNode, "version", "Unknown version" );

	if ( m_sSongVersion != QString( get_version().c_str() ) ) {
		WARNINGLOG( "Trying to load a song created with a different version of hydrogen." );
		WARNINGLOG( "Song [" + sFilename + "] saved with version " + m_sSongVersion );
	}

	float fBpm = LocalFileMng::readXmlFloat( songNode, "bpm", 120 );
	float fVolume = LocalFileMng::readXmlFloat( songNode, "volume", 0.5 );
	float fMetronomeVolume = LocalFileMng::readXmlFloat( songNode, "metronomeVolume", 0.5 );
	QString sName( LocalFileMng::readXmlString( songNode, "name", "Untitled Song" ) );
	QString sAuthor( LocalFileMng::readXmlString( songNode, "author", "Unknown Author" ) );
	QString sNotes( LocalFileMng::readXmlString( songNode, "notes", "..." ) );
	License license( LocalFileMng::readXmlString( songNode, "license", "" ) );
	bool bLoopEnabled = LocalFileMng::readXmlBool( songNode, "loopEnabled", false );
	bool bPatternMode =
		LocalFileMng::readXmlBool( songNode, "patternModeMode",
								   static_cast<bool>(Song::PatternMode::Selected) );
	
	Song::PatternMode patternMode = Song::PatternMode::Selected;
	if ( ! bPatternMode ) {
		patternMode = Song::PatternMode::Stacked;
	}
	Song::Mode mode = Song::Mode::Pattern;
	QString sMode = LocalFileMng::readXmlString( songNode, "mode", "pattern" );
	if ( sMode == "song" ) {
		mode = Song::Mode::Song;
	}

	QString sPlaybackTrack( LocalFileMng::readXmlString( songNode, "playbackTrackFilename", "" ) );
	bool bPlaybackTrackEnabled = LocalFileMng::readXmlBool( songNode, "playbackTrackEnabled", false );
	float fPlaybackTrackVolume = LocalFileMng::readXmlFloat( songNode, "playbackTrackVolume", 0.0 );

	// Check the file of the playback track and resort to the default
	// in case the file can not be found.
	if ( ! sPlaybackTrack.isEmpty() &&
		 ! Filesystem::file_exists( sPlaybackTrack, true ) ) {
		ERRORLOG( QString( "Provided playback track file [%1] does not exist. Using empty string instead" )
				  .arg( sPlaybackTrack ) );
		sPlaybackTrack = "";
	}

	Song::ActionMode actionMode = static_cast<Song::ActionMode>( LocalFileMng::readXmlInt( songNode, "action_mode",
																						   static_cast<int>( Song::ActionMode::selectMode ) ) );
	bool bIsPatternEditorLocked = LocalFileMng::readXmlBool( songNode, "isPatternEditorLocked", false );
	float fHumanizeTimeValue = LocalFileMng::readXmlFloat( songNode, "humanize_time", 0.0 );
	float fHumanizeVelocityValue = LocalFileMng::readXmlFloat( songNode, "humanize_velocity", 0.0 );
	float fSwingFactor = LocalFileMng::readXmlFloat( songNode, "swing_factor", 0.0 );
	bool bContainsIsTimelineActivated;
	bool bIsTimelineActivated =
		LocalFileMng::readXmlBool( songNode, "isTimelineActivated", false,
								   &bContainsIsTimelineActivated );
	if ( ! bContainsIsTimelineActivated ) {
		// .h2song file was created in an older version of
		// Hydrogen. Using the Timeline state in the
		// Preferences as a fallback.
		bIsTimelineActivated = pPreferences->getUseTimelineBpm();
	} else {
		pPreferences->setUseTimelineBpm( bIsTimelineActivated );
	}

	pSong = std::make_shared<Song>( sName, sAuthor, fBpm, fVolume );
	pSong->setMetronomeVolume( fMetronomeVolume );
	pSong->setNotes( sNotes );
	pSong->setLicense( license );
	if ( bLoopEnabled ) {
		pSong->setLoopMode( Song::LoopMode::Enabled );
	} else {
		pSong->setLoopMode( Song::LoopMode::Disabled );
	}
	pSong->setPatternMode( patternMode );
	pSong->setMode( mode );
	pSong->setHumanizeTimeValue( fHumanizeTimeValue );
	pSong->setHumanizeVelocityValue( fHumanizeVelocityValue );
	pSong->setSwingFactor( fSwingFactor );
	pSong->setPlaybackTrackFilename( sPlaybackTrack );
	pSong->setPlaybackTrackEnabled( bPlaybackTrackEnabled );
	pSong->setPlaybackTrackVolume( fPlaybackTrackVolume );
	pSong->setActionMode( actionMode );
	pSong->setIsPatternEditorLocked( bIsPatternEditorLocked );
	pSong->setIsTimelineActivated( bIsTimelineActivated );
	
	// pan law
	QString sPanLawType( LocalFileMng::readXmlString( songNode, "pan_law_type", "RATIO_STRAIGHT_POLYGONAL" ) );
	if ( sPanLawType == "RATIO_STRAIGHT_POLYGONAL" ) {
		pSong->setPanLawType( Sampler::RATIO_STRAIGHT_POLYGONAL );
	} else if ( sPanLawType == "RATIO_CONST_POWER" ) {
		pSong->setPanLawType( Sampler::RATIO_CONST_POWER );
	} else if ( sPanLawType == "RATIO_CONST_SUM" ) {
		pSong->setPanLawType( Sampler::RATIO_CONST_SUM );
	} else if ( sPanLawType == "LINEAR_STRAIGHT_POLYGONAL" ) {
		pSong->setPanLawType( Sampler::LINEAR_STRAIGHT_POLYGONAL );
	} else if ( sPanLawType == "LINEAR_CONST_POWER" ) {
		pSong->setPanLawType( Sampler::LINEAR_CONST_POWER );
	} else if ( sPanLawType == "LINEAR_CONST_SUM" ) {
		pSong->setPanLawType( Sampler::LINEAR_CONST_SUM );
	} else if ( sPanLawType == "POLAR_STRAIGHT_POLYGONAL" ) {
		pSong->setPanLawType( Sampler::POLAR_STRAIGHT_POLYGONAL );
	} else if ( sPanLawType == "POLAR_CONST_POWER" ) {
		pSong->setPanLawType( Sampler::POLAR_CONST_POWER );
	} else if ( sPanLawType == "POLAR_CONST_SUM" ) {
		pSong->setPanLawType( Sampler::POLAR_CONST_SUM );
	} else if ( sPanLawType == "QUADRATIC_STRAIGHT_POLYGONAL" ) {
		pSong->setPanLawType( Sampler::QUADRATIC_STRAIGHT_POLYGONAL );
	} else if ( sPanLawType == "QUADRATIC_CONST_POWER" ) {
		pSong->setPanLawType( Sampler::QUADRATIC_CONST_POWER );
	} else if ( sPanLawType == "QUADRATIC_CONST_SUM" ) {
		pSong->setPanLawType( Sampler::QUADRATIC_CONST_SUM );
	} else if ( sPanLawType == "LINEAR_CONST_K_NORM" ) {
		pSong->setPanLawType( Sampler::LINEAR_CONST_K_NORM );
	} else if ( sPanLawType == "POLAR_CONST_K_NORM" ) {
		pSong->setPanLawType( Sampler::POLAR_CONST_K_NORM );
	} else if ( sPanLawType == "RATIO_CONST_K_NORM" ) {
		pSong->setPanLawType( Sampler::RATIO_CONST_K_NORM );
	} else if ( sPanLawType == "QUADRATIC_CONST_K_NORM" ) {
		pSong->setPanLawType( Sampler::QUADRATIC_CONST_K_NORM );
	} else {
		pSong->setPanLawType( Sampler::RATIO_STRAIGHT_POLYGONAL );
		WARNINGLOG( "Unknown pan law type in import song. Set default." );
	}

	float fPanLawKNorm = LocalFileMng::readXmlFloat( songNode, "pan_law_k_norm", Sampler::K_NORM_DEFAULT );
	if ( fPanLawKNorm <= 0.0 ) {
		fPanLawKNorm = Sampler::K_NORM_DEFAULT;
		WARNINGLOG( "Invalid pan law k in import song (<= 0). Set default k." );
	}
	pSong->setPanLawKNorm( fPanLawKNorm );

	QDomNode componentListNode = songNode.firstChildElement( "componentList" );
	if ( ( ! componentListNode.isNull()  ) ) {
		QDomNode componentNode = componentListNode.firstChildElement( "drumkitComponent" );
		while ( ! componentNode.isNull()  ) {
			int id = LocalFileMng::readXmlInt( componentNode, "id", -1 );			// instrument id
			QString sName = LocalFileMng::readXmlString( componentNode, "name", "" );		// name
			float fVolume = LocalFileMng::readXmlFloat( componentNode, "volume", 1.0 );	// volume
			
			DrumkitComponent* pDrumkitComponent = new DrumkitComponent( id, sName );
			pDrumkitComponent->set_volume( fVolume );

			pSong->getComponents()->push_back(pDrumkitComponent);

			componentNode = ( QDomNode ) componentNode.nextSiblingElement( "drumkitComponent" );
		}
	} else {
		DrumkitComponent* pDrumkitComponent = new DrumkitComponent( 0, "Main" );
		pSong->getComponents()->push_back(pDrumkitComponent);
	}

	//  Instrument List
	InstrumentList* pInstrList = new InstrumentList();

	QDomNode instrumentListNode = songNode.firstChildElement( "instrumentList" );
	if ( ( ! instrumentListNode.isNull()  ) ) {
		// INSTRUMENT NODE
		int instrumentList_count = 0;
		QDomNode instrumentNode;
		instrumentNode = instrumentListNode.firstChildElement( "instrument" );
		while ( ! instrumentNode.isNull()  ) {
			instrumentList_count++;

			int id = LocalFileMng::readXmlInt( instrumentNode, "id", -1 );			// instrument id
			QString sDrumkit = LocalFileMng::readXmlString( instrumentNode, "drumkit", "" );	// drumkit
			pSong->setCurrentDrumkitName( sDrumkit );
			int iLookup = LocalFileMng::readXmlInt( instrumentNode, "drumkitLookup", -1 );	// drumkit
			if ( iLookup == -1 ) {
				// Song was created with an older version of the
				// Hydrogen and we just have the name of the drumkit
				// and no information whether it is found at user- or
				// system-level.

				if ( ! Filesystem::drumkit_path_search( sDrumkit, Filesystem::Lookup::user, true ).isEmpty() ) {
					iLookup = 1;
					WARNINGLOG( "Missing drumkitLookup: user-level determined" );
				} else if ( ! Filesystem::drumkit_path_search( sDrumkit, Filesystem::Lookup::system, true ).isEmpty() ) {
					iLookup = 2;
					WARNINGLOG( "Missing drumkitLookup: system-level determined" );
				} else {
					iLookup = 2;
					ERRORLOG( "Missing drumkitLookup: drumkit could not be found. system-level will be set as a fallback" );
				}
			}	
			pSong->setCurrentDrumkitLookup( static_cast<Filesystem::Lookup>( iLookup ) );
			QString sName = LocalFileMng::readXmlString( instrumentNode, "name", "" );		// name
			float fVolume = LocalFileMng::readXmlFloat( instrumentNode, "volume", 1.0 );	// volume
			bool bIsMuted = LocalFileMng::readXmlBool( instrumentNode, "isMuted", false );	// is muted
			bool bIsSoloed = LocalFileMng::readXmlBool( instrumentNode, "isSoloed", false );	// is soloed
			
			bool bFound, bFound2;
			float fPan = LocalFileMng::readXmlFloat( instrumentNode, "pan", 0.f, &bFound );
			if ( !bFound ) {
				// check if pan is expressed in the old fashion (version <= 1.1 ) with the pair (pan_L, pan_R)
				float fPanL = LocalFileMng::readXmlFloat( instrumentNode, "pan_L", 1.f, &bFound );
				float fPanR = LocalFileMng::readXmlFloat( instrumentNode, "pan_R", 1.f, &bFound2 );
				if ( bFound == true && bFound2 == true ) { // found nodes pan_L and pan_R
					fPan = Sampler::getRatioPan( fPanL, fPanR );  // convert to single pan parameter
				}
			}

			float fFX1Level = LocalFileMng::readXmlFloat( instrumentNode, "FX1Level", 0.0 );	// FX level
			float fFX2Level = LocalFileMng::readXmlFloat( instrumentNode, "FX2Level", 0.0 );	// FX level
			float fFX3Level = LocalFileMng::readXmlFloat( instrumentNode, "FX3Level", 0.0 );	// FX level
			float fFX4Level = LocalFileMng::readXmlFloat( instrumentNode, "FX4Level", 0.0 );	// FX level
			float fGain = LocalFileMng::readXmlFloat( instrumentNode, "gain", 1.0, false, false );	// instrument gain

			int fAttack = LocalFileMng::readXmlInt( instrumentNode, "Attack", 0, false, false );		// Attack
			int fDecay = LocalFileMng::readXmlInt( instrumentNode, "Decay", 0, false, false );		// Decay
			float fSustain = LocalFileMng::readXmlFloat( instrumentNode, "Sustain", 1.0, false, false );	// Sustain
			int fRelease = LocalFileMng::readXmlFloat( instrumentNode, "Release", 1000.0, false, false );	// Release
			
			float fPitchOffset = LocalFileMng::readXmlFloat( instrumentNode, "pitchOffset", 0.0f, false, false );
			float fRandomPitchFactor = LocalFileMng::readXmlFloat( instrumentNode, "randomPitchFactor", 0.0f, false, false );

			bool bApplyVelocity = LocalFileMng::readXmlBool( instrumentNode, "applyVelocity", true );
			bool bFilterActive = LocalFileMng::readXmlBool( instrumentNode, "filterActive", false );
			float fFilterCutoff = LocalFileMng::readXmlFloat( instrumentNode, "filterCutoff", 1.0f, false );
			float fFilterResonance = LocalFileMng::readXmlFloat( instrumentNode, "filterResonance", 0.0f, false );
			QString sMuteGroup = LocalFileMng::readXmlString( instrumentNode, "muteGroup", "-1", false );
			QString sMidiOutChannel = LocalFileMng::readXmlString( instrumentNode, "midiOutChannel", "-1", false, false );
			QString sMidiOutNote = LocalFileMng::readXmlString( instrumentNode, "midiOutNote", "60", false, false );
			int nMuteGroup = sMuteGroup.toInt();
			bool isStopNote = LocalFileMng::readXmlBool( instrumentNode, "isStopNote", false );
			QString sRead_sample_select_algo = LocalFileMng::readXmlString( instrumentNode, "sampleSelectionAlgo", "VELOCITY" );

			int nMidiOutChannel = sMidiOutChannel.toInt();
			int nMidiOutNote = sMidiOutNote.toInt();

			if ( id==-1 ) {
				ERRORLOG( "Empty ID for instrument '" + sName + "'. skipping." );
				instrumentNode = ( QDomNode ) instrumentNode.nextSiblingElement( "instrument" );
				continue;
			}

			int iIsHiHat = LocalFileMng::readXmlInt( instrumentNode, "isHihat", -1, true );
			int iLowerCC = LocalFileMng::readXmlInt( instrumentNode, "lower_cc", 0, true );
			int iHigherCC = LocalFileMng::readXmlInt( instrumentNode, "higher_cc", 127, true );

			// create a new instrument
			auto pInstrument = std::make_shared<Instrument>( id, sName, std::make_shared<ADSR>( fAttack, fDecay, fSustain, fRelease ) );
			pInstrument->set_volume( fVolume );
			pInstrument->set_muted( bIsMuted );
			pInstrument->set_soloed( bIsSoloed );
			pInstrument->setPan( fPan );
			pInstrument->set_drumkit_name( sDrumkit );
			pInstrument->set_apply_velocity( bApplyVelocity );
			pInstrument->set_fx_level( fFX1Level, 0 );
			pInstrument->set_fx_level( fFX2Level, 1 );
			pInstrument->set_fx_level( fFX3Level, 2 );
			pInstrument->set_fx_level( fFX4Level, 3 );
			pInstrument->set_pitch_offset( fPitchOffset );
			pInstrument->set_random_pitch_factor( fRandomPitchFactor );
			pInstrument->set_filter_active( bFilterActive );
			pInstrument->set_filter_cutoff( fFilterCutoff );
			pInstrument->set_filter_resonance( fFilterResonance );
			pInstrument->set_gain( fGain );
			pInstrument->set_mute_group( nMuteGroup );
			pInstrument->set_stop_notes( isStopNote );
			pInstrument->set_hihat_grp( iIsHiHat );
			pInstrument->set_lower_cc( iLowerCC );
			pInstrument->set_higher_cc( iHigherCC );
			if ( sRead_sample_select_algo.compare("VELOCITY") == 0 ) {
				pInstrument->set_sample_selection_alg( Instrument::VELOCITY );
			} else if ( sRead_sample_select_algo.compare("ROUND_ROBIN") == 0 ) {
				pInstrument->set_sample_selection_alg( Instrument::ROUND_ROBIN );
			} else if ( sRead_sample_select_algo.compare("RANDOM") == 0 ) {
				pInstrument->set_sample_selection_alg( Instrument::RANDOM );
			}
			pInstrument->set_midi_out_channel( nMidiOutChannel );
			pInstrument->set_midi_out_note( nMidiOutNote );

			QString drumkitPath;
			if ( ( !sDrumkit.isEmpty() ) && ( sDrumkit != "-" ) ) {
				drumkitPath = Filesystem::drumkit_path_search( sDrumkit );
			} else {
				ERRORLOG( "Missing drumkit path" );
			}

			// Get license used by drumkit.
			Drumkit* pDrumkit = Drumkit::load( drumkitPath, false, false, false );
			License drumkitLicense;
			if ( pDrumkit != nullptr ) {
				drumkitLicense = pDrumkit->get_license();
				delete pDrumkit;
			}

			QDomNode sFilenameNode = instrumentNode.firstChildElement( "filename" );

			// back compatibility code ( song version <= 0.9.0 )
			if ( ! sFilenameNode.isNull() ) {
				WARNINGLOG( "Using back compatibility code. sFilename node found" );
				QString sFilename = LocalFileMng::readXmlString( instrumentNode, "filename", "" );

				if ( !QFile( sFilename ).exists() && !drumkitPath.isEmpty() ) {
					sFilename = drumkitPath + "/" + sFilename;
				}
				auto pSample = Sample::load( sFilename, drumkitLicense );
				if ( pSample == nullptr ) {
					// nel passaggio tra 0.8.2 e 0.9.0 il drumkit di default e' cambiato.
					// Se fallisce provo a caricare il corrispettivo file in formato flac
//					warningLog( "[readSong] Error loading sample: " + sFilename + " not found. Trying to load a flac..." );
					sFilename = sFilename.left( sFilename.length() - 4 );
					sFilename += ".flac";
					pSample = Sample::load( sFilename, drumkitLicense );
				}
				if ( pSample == nullptr ) {
					ERRORLOG( "Error loading sample: " + sFilename + " not found" );
					pInstrument->set_muted( true );
					pInstrument->set_missing_samples( true );
				}
				auto pCompo = std::make_shared<InstrumentComponent>( 0 );
				auto pLayer = std::make_shared<InstrumentLayer>( pSample );
				pCompo->set_layer( pLayer, 0 );
				pInstrument->get_components()->push_back( pCompo );
			}
			//~ back compatibility code
			else {
				bool bFoundAtLeastOneComponent = false;
				QDomNode componentNode = instrumentNode.firstChildElement( "instrumentComponent" );
				while (  ! componentNode.isNull()  ) {
					bFoundAtLeastOneComponent = true;
					int id = LocalFileMng::readXmlInt( componentNode, "component_id", 0 );
					auto pCompo = std::make_shared<InstrumentComponent>( id );
					float fGainCompo = LocalFileMng::readXmlFloat( componentNode, "gain", 1.0 );
					pCompo->set_gain( fGainCompo );

					unsigned nLayer = 0;
					QDomNode layerNode = componentNode.firstChildElement( "layer" );
					while (  ! layerNode.isNull()  ) {
						if ( nLayer >= InstrumentComponent::getMaxLayers() ) {
							ERRORLOG( QString( "nLayer (%1) > m_nMaxLayers (%2)" ).arg ( nLayer ).arg( InstrumentComponent::getMaxLayers() ) );
							continue;
						}
						//bool sIsModified = false;
						QString sFilename = LocalFileMng::readXmlString( layerNode, "filename", "" );
						bool sIsModified = LocalFileMng::readXmlBool( layerNode, "ismodified", false );
						Sample::Loops lo;
						lo.mode = Sample::parse_loop_mode( LocalFileMng::readXmlString( layerNode, "smode", "forward" ) );
						lo.start_frame = LocalFileMng::readXmlInt( layerNode, "startframe", 0 );
						lo.loop_frame = LocalFileMng::readXmlInt( layerNode, "loopframe", 0 );
						lo.count = LocalFileMng::readXmlInt( layerNode, "loops", 0 );
						lo.end_frame = LocalFileMng::readXmlInt( layerNode, "endframe", 0 );
						Sample::Rubberband ro;
						ro.use = LocalFileMng::readXmlInt( layerNode, "userubber", 0, false );
						ro.divider = LocalFileMng::readXmlFloat( layerNode, "rubberdivider", 0.0 );
						ro.c_settings = LocalFileMng::readXmlInt( layerNode, "rubberCsettings", 1 );
						ro.pitch = LocalFileMng::readXmlFloat( layerNode, "rubberPitch", 0.0 );

						float fMin = LocalFileMng::readXmlFloat( layerNode, "min", 0.0 );
						float fMax = LocalFileMng::readXmlFloat( layerNode, "max", 1.0 );
						float fGain = LocalFileMng::readXmlFloat( layerNode, "gain", 1.0 );
						float fPitch = LocalFileMng::readXmlFloat( layerNode, "pitch", 0.0, false, false );

						if ( !QFile( sFilename ).exists() && !drumkitPath.isEmpty() && !sFilename.startsWith("/")) {
							sFilename = drumkitPath + "/" + sFilename;
						}

						QString program = pPreferences->m_rubberBandCLIexecutable;
						//test the path. if test fails, disable rubberband
						if ( QFile( program ).exists() == false ) {
							ro.use = false;
						}

						std::shared_ptr<Sample> pSample;
						if ( !sIsModified ) {
							pSample = Sample::load( sFilename, drumkitLicense );
						} else {
							// FIXME, kill EnvelopePoint, create Envelope class
							EnvelopePoint pt;

							Sample::VelocityEnvelope velocity;
							QDomNode volumeNode = layerNode.firstChildElement( "volume" );
							while (  ! volumeNode.isNull()  ) {
								pt.frame = LocalFileMng::readXmlInt( volumeNode, "volume-position", 0 );
								pt.value = LocalFileMng::readXmlInt( volumeNode, "volume-value", 0 );
								velocity.push_back( pt );
								volumeNode = volumeNode.nextSiblingElement( "volume" );
								//ERRORLOG( QString("volume-posi %1").arg(LocalFileMng::readXmlInt( volumeNode, "volume-position", 0)) );
							}

							Sample::VelocityEnvelope pan;
							QDomNode  panNode = layerNode.firstChildElement( "pan" );
							while (  ! panNode.isNull()  ) {
								pt.frame = LocalFileMng::readXmlInt( panNode, "pan-position", 0 );
								pt.value = LocalFileMng::readXmlInt( panNode, "pan-value", 0 );
								pan.push_back( pt );
								panNode = panNode.nextSiblingElement( "pan" );
							}

							pSample = Sample::load( sFilename, lo, ro, velocity,
													pan, fBpm, drumkitLicense );
						}
						if ( pSample == nullptr ) {
							ERRORLOG( "Error loading sample: " + sFilename + " not found" );
							pInstrument->set_muted( true );
							pInstrument->set_missing_samples( true );
						}
						auto pLayer = std::make_shared<InstrumentLayer>( pSample );
						pLayer->set_start_velocity( fMin );
						pLayer->set_end_velocity( fMax );
						pLayer->set_gain( fGain );
						pLayer->set_pitch( fPitch );
						pCompo->set_layer( pLayer, nLayer );
						nLayer++;

						layerNode = ( QDomNode ) layerNode.nextSiblingElement( "layer" );
					}

					pInstrument->get_components()->push_back( pCompo );
					componentNode = ( QDomNode ) componentNode.nextSiblingElement( "instrumentComponent" );
				}
				if(!bFoundAtLeastOneComponent) {
					auto pCompo = std::make_shared<InstrumentComponent>( 0 );
					float fGainCompo = LocalFileMng::readXmlFloat( componentNode, "gain", 1.0 );
					pCompo->set_gain( fGainCompo );

					unsigned nLayer = 0;
					QDomNode layerNode = instrumentNode.firstChildElement( "layer" );
					while (  ! layerNode.isNull()  ) {
						if ( nLayer >= InstrumentComponent::getMaxLayers() ) {
							ERRORLOG( QString( "nLayer (%1) > m_nMaxLayers (%2)" ).arg ( nLayer ).arg( InstrumentComponent::getMaxLayers() ) );
							continue;
						}
						QString sFilename = LocalFileMng::readXmlString( layerNode, "filename", "" );
						bool sIsModified = LocalFileMng::readXmlBool( layerNode, "ismodified", false );
						Sample::Loops lo;
						lo.mode = Sample::parse_loop_mode( LocalFileMng::readXmlString( layerNode, "smode", "forward" ) );
						lo.start_frame = LocalFileMng::readXmlInt( layerNode, "startframe", 0 );
						lo.loop_frame = LocalFileMng::readXmlInt( layerNode, "loopframe", 0 );
						lo.count = LocalFileMng::readXmlInt( layerNode, "loops", 0 );
						lo.end_frame = LocalFileMng::readXmlInt( layerNode, "endframe", 0 );
						Sample::Rubberband ro;
						ro.use = LocalFileMng::readXmlInt( layerNode, "userubber", 0, false );
						ro.divider = LocalFileMng::readXmlFloat( layerNode, "rubberdivider", 0.0 );
						ro.c_settings = LocalFileMng::readXmlInt( layerNode, "rubberCsettings", 1 );
						ro.pitch = LocalFileMng::readXmlFloat( layerNode, "rubberPitch", 0.0 );

						float fMin = LocalFileMng::readXmlFloat( layerNode, "min", 0.0 );
						float fMax = LocalFileMng::readXmlFloat( layerNode, "max", 1.0 );
						float fGain = LocalFileMng::readXmlFloat( layerNode, "gain", 1.0 );
						float fPitch = LocalFileMng::readXmlFloat( layerNode, "pitch", 0.0, false, false );

						if ( !QFile( sFilename ).exists() && !drumkitPath.isEmpty() ) {
							sFilename = drumkitPath + "/" + sFilename;
						}

						QString program = pPreferences->m_rubberBandCLIexecutable;
						//test the path. if test fails, disable rubberband
						if ( QFile( program ).exists() == false ) {
							ro.use = false;
						}

						std::shared_ptr<Sample> pSample = nullptr;
						if ( !sIsModified ) {
							pSample = Sample::load( sFilename );
						} else {
							EnvelopePoint pt;

							Sample::VelocityEnvelope velocity;
							QDomNode volumeNode = layerNode.firstChildElement( "volume" );
							while (  ! volumeNode.isNull()  ) {
								pt.frame = LocalFileMng::readXmlInt( volumeNode, "volume-position", 0 );
								pt.value = LocalFileMng::readXmlInt( volumeNode, "volume-value", 0 );
								velocity.push_back( pt );
								volumeNode = volumeNode.nextSiblingElement( "volume" );
								//ERRORLOG( QString("volume-posi %1").arg(LocalFileMng::readXmlInt( volumeNode, "volume-position", 0)) );
							}

							Sample::VelocityEnvelope pan;
							QDomNode  panNode = layerNode.firstChildElement( "pan" );
							while (  ! panNode.isNull()  ) {
								pt.frame = LocalFileMng::readXmlInt( panNode, "pan-position", 0 );
								pt.value = LocalFileMng::readXmlInt( panNode, "pan-value", 0 );
								pan.push_back( pt );
								panNode = panNode.nextSiblingElement( "pan" );
							}

							pSample = Sample::load( sFilename, lo, ro, velocity, pan, fBpm );
						}
						if ( pSample == nullptr ) {
							ERRORLOG( "Error loading sample: " + sFilename + " not found" );
							pInstrument->set_muted( true );
							pInstrument->set_missing_samples( true );
						}
						auto pLayer = std::make_shared<InstrumentLayer>( pSample );
						pLayer->set_start_velocity( fMin );
						pLayer->set_end_velocity( fMax );
						pLayer->set_gain( fGain );
						pLayer->set_pitch( fPitch );
						pCompo->set_layer( pLayer, nLayer );
						nLayer++;

						layerNode = ( QDomNode ) layerNode.nextSiblingElement( "layer" );
					}
					pInstrument->get_components()->push_back( pCompo );
				}
			}
			pInstrList->add( pInstrument );
			instrumentNode = ( QDomNode ) instrumentNode.nextSiblingElement( "instrument" );
		}

		if ( instrumentList_count == 0 ) {
			WARNINGLOG( "0 instruments?" );
		}
		pSong->setInstrumentList( pInstrList );
	} else {
		ERRORLOG( "Error reading song: instrumentList node not found" );
		delete pInstrList;
		return nullptr;
	}

	// Pattern list
	QDomNode patterns = songNode.firstChildElement( "patternList" );

	PatternList* pPatternList = new PatternList();
	int pattern_count = 0;

	QDomNode patternNode =  patterns.firstChildElement( "pattern" );
	while (  !patternNode.isNull()  ) {
		pattern_count++;
		Pattern* pPattern = getPattern( patternNode, pInstrList );
		if ( pPattern ) {
			pPatternList->add( pPattern );
		} else {
			ERRORLOG( "Error loading pattern" );
			delete pPatternList;
			return nullptr;
		}
		patternNode = ( QDomNode ) patternNode.nextSiblingElement( "pattern" );
	}
	if ( pattern_count == 0 ) {
		WARNINGLOG( "0 patterns?" );
	}
	pSong->setPatternList( pPatternList );

	// Virtual Patterns
	QDomNode  virtualPatternListNode = songNode.firstChildElement( "virtualPatternList" );
	QDomNode virtualPatternNode = virtualPatternListNode.firstChildElement( "pattern" );
	if ( !virtualPatternNode.isNull() ) {

		while (  ! virtualPatternNode.isNull()  ) {
			QString sName = "";
			sName = LocalFileMng::readXmlString( virtualPatternNode, "name", sName );

			Pattern* pCurPattern = nullptr;
			unsigned nPatterns = pPatternList->size();
			for ( unsigned i = 0; i < nPatterns; i++ ) {
				Pattern* pPattern = pPatternList->get( i );

				if ( pPattern->get_name() == sName ) {
					pCurPattern = pPattern;
					break;
				}//if
			}//for

			if ( pCurPattern != nullptr ) {
				QDomNode  virtualNode = virtualPatternNode.firstChildElement( "virtual" );
				while (  !virtualNode.isNull()  ) {
					QString virtName = virtualNode.firstChild().nodeValue();

					Pattern* virtPattern = nullptr;
					for ( unsigned i = 0; i < nPatterns; i++ ) {
						Pattern* pat = pPatternList->get( i );

						if ( pat->get_name() == virtName ) {
							virtPattern = pat;
							break;
						}//if
					}//for

					if ( virtPattern != nullptr ) {
						pCurPattern->virtual_patterns_add( virtPattern );
					} else {
						ERRORLOG( "Song had invalid virtual pattern list data (virtual)" );
					}//if
					virtualNode = ( QDomNode ) virtualNode.nextSiblingElement( "virtual" );
				}//while
			} else {
				ERRORLOG( "Song had invalid virtual pattern list data (name)" );
			}//if
			virtualPatternNode = ( QDomNode ) virtualPatternNode.nextSiblingElement( "pattern" );
		}//while
	}//if

	pPatternList->flattened_virtual_patterns_compute();

	// Pattern sequence
	QDomNode patternSequenceNode = songNode.firstChildElement( "patternSequence" );

	std::vector<PatternList*>* pPatternGroupVector = new std::vector<PatternList*>;

	// back-compatibility code..
	QDomNode pPatternIDNode = patternSequenceNode.firstChildElement( "patternID" );
	while ( ! pPatternIDNode.isNull()  ) {
		WARNINGLOG( "Using old patternSequence code for back compatibility" );
		PatternList* pPatternSequence = new PatternList();
		QString patId = pPatternIDNode.firstChildElement().text();
		ERRORLOG( patId );

		Pattern* pPattern = nullptr;
		for ( unsigned i = 0; i < pPatternList->size(); i++ ) {
			Pattern* pTmpPattern = pPatternList->get( i );
			if ( pTmpPattern ) {
				if ( pTmpPattern->get_name() == patId ) {
					pPattern = pTmpPattern;
					break;
				}
			}
		}
		if ( pPattern == nullptr ) {
			WARNINGLOG( "patternid not found in patternSequence" );
			pPatternIDNode = ( QDomNode ) pPatternIDNode.nextSiblingElement( "patternID" );
			
			delete pPatternSequence;
			
			continue;
		}
		pPatternSequence->add( pPattern );

		pPatternGroupVector->push_back( pPatternSequence );

		pPatternIDNode = ( QDomNode ) pPatternIDNode.nextSiblingElement( "patternID" );
	}

	QDomNode groupNode = patternSequenceNode.firstChildElement( "group" );
	while (  !groupNode.isNull()  ) {
		PatternList* patternSequence = new PatternList();
		QDomNode patternId = groupNode.firstChildElement( "patternID" );
		while (  !patternId.isNull()  ) {
			QString patId = patternId.firstChild().nodeValue();

			Pattern* pPattern = nullptr;
			for ( unsigned i = 0; i < pPatternList->size(); i++ ) {
				Pattern* pTmpPattern = pPatternList->get( i );
				if ( pTmpPattern ) {
					if ( pTmpPattern->get_name() == patId ) {
						pPattern = pTmpPattern;
						break;
					}
				}
			}
			if ( pPattern == nullptr ) {
				WARNINGLOG( "patternid not found in patternSequence" );
				patternId = ( QDomNode ) patternId.nextSiblingElement( "patternID" );
				continue;
			}
			patternSequence->add( pPattern );
			patternId = ( QDomNode ) patternId.nextSiblingElement( "patternID" );
		}
		pPatternGroupVector->push_back( patternSequence );

		groupNode = groupNode.nextSiblingElement( "group" );
	}

	pSong->setPatternGroupVector( pPatternGroupVector );

#ifdef H2CORE_HAVE_LADSPA
	// reset FX
	for ( int fx = 0; fx < MAX_FX; ++fx ) {
		//LadspaFX* pFX = Effects::get_instance()->getLadspaFX( fx );
		//delete pFX;
		Effects::get_instance()->setLadspaFX( nullptr, fx );
	}
#endif

	// LADSPA FX
	QDomNode ladspaNode = songNode.firstChildElement( "ladspa" );
	if ( !ladspaNode.isNull() ) {
		int nFX = 0;
		QDomNode fxNode = ladspaNode.firstChildElement( "fx" );
		while (  !fxNode.isNull()  ) {
			QString sName = LocalFileMng::readXmlString( fxNode, "name", "" );
			QString sFilename = LocalFileMng::readXmlString( fxNode, "filename", "" );
			bool bEnabled = LocalFileMng::readXmlBool( fxNode, "enabled", false );
			float fVolume = LocalFileMng::readXmlFloat( fxNode, "volume", 1.0 );

			if ( sName != "no plugin" ) {
				// FIXME: il caricamento va fatto fare all'engine, solo lui sa il samplerate esatto
#ifdef H2CORE_HAVE_LADSPA
				LadspaFX* pFX = LadspaFX::load( sFilename, sName, 44100 );
				Effects::get_instance()->setLadspaFX( pFX, nFX );
				if ( pFX ) {
					pFX->setEnabled( bEnabled );
					pFX->setVolume( fVolume );
					QDomNode inputControlNode = fxNode.firstChildElement( "inputControlPort" );
					while ( !inputControlNode.isNull() ) {
						QString sName = LocalFileMng::readXmlString( inputControlNode, "name", "" );
						float fValue = LocalFileMng::readXmlFloat( inputControlNode, "value", 0.0 );

						for ( unsigned nPort = 0; nPort < pFX->inputControlPorts.size(); nPort++ ) {
							LadspaControlPort* port = pFX->inputControlPorts[ nPort ];
							if ( QString( port->sName ) == sName ) {
								port->fControlValue = fValue;
							}
						}
						inputControlNode = ( QDomNode ) inputControlNode.nextSiblingElement( "inputControlPort" );
					}
				}
#endif
			}
			nFX++;
			fxNode = ( QDomNode ) fxNode.nextSiblingElement( "fx" );
		}
	} else {
		WARNINGLOG( "ladspa node not found" );
	}

	std::shared_ptr<Timeline> pTimeline = std::make_shared<Timeline>();
	QDomNode bpmTimeLine = songNode.firstChildElement( "BPMTimeLine" );
	if ( !bpmTimeLine.isNull() ) {
		QDomNode newBPMNode = bpmTimeLine.firstChildElement( "newBPM" );
		while( !newBPMNode.isNull() ) {
			pTimeline->addTempoMarker( LocalFileMng::readXmlInt( newBPMNode, "BAR", 0 ),
									   LocalFileMng::readXmlFloat( newBPMNode, "BPM", 120.0 ) );
			newBPMNode = newBPMNode.nextSiblingElement( "newBPM" );
		}
	} else {
		WARNINGLOG( "bpmTimeLine node not found" );
	}

	QDomNode timeLineTag = songNode.firstChildElement( "timeLineTag" );
	if ( !timeLineTag.isNull() ) {
		QDomNode newTAGNode = timeLineTag.firstChildElement( "newTAG" );
		while( !newTAGNode.isNull() ) {
			pTimeline->addTag( LocalFileMng::readXmlInt( newTAGNode, "BAR", 0 ),
							   LocalFileMng::readXmlString( newTAGNode, "TAG", "" ) );
			newTAGNode = newTAGNode.nextSiblingElement( "newTAG" );
		}
	} else {
		WARNINGLOG( "TagTimeLine node not found" );
	}
	pSong->setTimeline( pTimeline );

	// Automation Paths
	QDomNode automationPathsNode = songNode.firstChildElement( "automationPaths" );
	if ( !automationPathsNode.isNull() ) {
		AutomationPathSerializer pathSerializer;

		QDomElement pathNode = automationPathsNode.firstChildElement( "path" );
		while( !pathNode.isNull()) {
			QString sAdjust = pathNode.attribute( "adjust" );

			// Select automation path to be read based on "adjust" attribute
			AutomationPath *pPath = nullptr;
			if (sAdjust == "velocity") {
				pPath = pSong->getVelocityAutomationPath();
			}

			if (pPath) {
				pathSerializer.read_automation_path( pathNode, *pPath );
			}

			pathNode = pathNode.nextSiblingElement( "path" );
		}
	}

	pSong->setFilename( sFilename );
	pSong->setIsModified( false );

	return pSong;
}

Pattern* SongReader::getPattern( QDomNode pattern, InstrumentList* pInstrList )
{
	Pattern* pPattern = nullptr;

	QString sName;	// name
	sName = LocalFileMng::readXmlString( pattern, "name", sName );
	QString sInfo;
	sInfo = LocalFileMng::readXmlString( pattern, "info", sInfo,false,false );
	QString sCategory; // category
	sCategory = LocalFileMng::readXmlString( pattern, "category", sCategory,false,false );

	int nSize = -1;
	nSize = LocalFileMng::readXmlInt( pattern, "size", nSize, false, false );
	int nDenominator = 4;
	nDenominator = LocalFileMng::readXmlInt( pattern, "denominator", nDenominator, false, false );

	pPattern = new Pattern( sName, sInfo, sCategory, nSize, nDenominator );

	QDomNode pNoteListNode = pattern.firstChildElement( "noteList" );
	if ( ! pNoteListNode.isNull() ) {
		// new code :)
		QDomNode noteNode = pNoteListNode.firstChildElement( "note" );
		while ( ! noteNode.isNull()  ) {

			Note* pNote = nullptr;

			unsigned nPosition = LocalFileMng::readXmlInt( noteNode, "position", 0 );
			float fLeadLag = LocalFileMng::readXmlFloat( noteNode, "leadlag", 0.0, false, false );
			float fVelocity = LocalFileMng::readXmlFloat( noteNode, "velocity", 0.8f );
			
			bool bFound, bFound2;
			float fPan = LocalFileMng::readXmlFloat( noteNode, "pan", 0.f, &bFound );
			if ( !bFound ) {
				// check if pan is expressed in the old fashion (version <= 1.1 ) with the couple (pan_L, pan_R)
				float fPanL = LocalFileMng::readXmlFloat( noteNode, "pan_L", 1.f, &bFound );
				float fPanR = LocalFileMng::readXmlFloat( noteNode, "pan_R", 1.f, &bFound2 );
				if ( bFound == true && bFound2 == true ) { // found nodes pan_L and pan_R
					fPan = Sampler::getRatioPan( fPanL, fPanR );  // convert to single pan parameter
				}
			}
			
			int nLength = LocalFileMng::readXmlInt( noteNode, "length", -1, true );
			float nPitch = LocalFileMng::readXmlFloat( noteNode, "pitch", 0.0, false, false );
			float fProbability = LocalFileMng::readXmlFloat( noteNode, "probability", 1.0, false, false );
			QString sKey = LocalFileMng::readXmlString( noteNode, "key", "C0", false, false );
			QString nNoteOff = LocalFileMng::readXmlString( noteNode, "note_off", "false", false, false );

			int instrId = LocalFileMng::readXmlInt( noteNode, "instrument", -1 );

			std::shared_ptr<Instrument> pInstrumentRef = nullptr;
			// search instrument by ref
			pInstrumentRef = pInstrList->find( instrId );
			if ( !pInstrumentRef ) {
				ERRORLOG( QString( "Instrument with ID: '%1' not found. Note skipped." ).arg( instrId ) );
				noteNode = ( QDomNode ) noteNode.nextSiblingElement( "note" );
				continue;
			}
			//assert( instrRef );
			bool noteoff = false;
			if ( nNoteOff == "true" ) {
				noteoff = true;
			}

			pNote = new Note( pInstrumentRef, nPosition, fVelocity, fPan, nLength, nPitch );
			pNote->set_key_octave( sKey );
			pNote->set_lead_lag( fLeadLag );
			pNote->set_note_off( noteoff );
			pNote->set_probability( fProbability );
			pPattern->insert_note( pNote );

			noteNode = ( QDomNode ) noteNode.nextSiblingElement( "note" );
		}
	} else {
		// Back compatibility code. Version < 0.9.4
		QDomNode sequenceListNode = pattern.firstChildElement( "sequenceList" );

		int sequence_count = 0;
		QDomNode sequenceNode = sequenceListNode.firstChildElement( "sequence" );
		while ( ! sequenceNode.isNull()  ) {
			sequence_count++;

			QDomNode noteListNode = sequenceNode.firstChildElement( "noteList" );
			QDomNode noteNode = noteListNode.firstChildElement( "note" );
			while (  !noteNode.isNull() ) {
				Note* pNote = nullptr;

				unsigned nPosition = LocalFileMng::readXmlInt( noteNode, "position", 0 );
				float fLeadLag = LocalFileMng::readXmlFloat( noteNode, "leadlag", 0.0, false, false );
				float fVelocity = LocalFileMng::readXmlFloat( noteNode, "velocity", 0.8f );
				float fPanL = LocalFileMng::readXmlFloat( noteNode, "pan_L", 0.5 );
				float fPanR = LocalFileMng::readXmlFloat( noteNode, "pan_R", 0.5 );
				float fPan = Sampler::getRatioPan( fPanL, fPanR ); // convert to single pan parameter
				int nLength = LocalFileMng::readXmlInt( noteNode, "length", -1, true );
				float nPitch = LocalFileMng::readXmlFloat( noteNode, "pitch", 0.0, false, false );

				int instrId = LocalFileMng::readXmlInt( noteNode, "instrument", -1 );

				auto instrRef = pInstrList->find( instrId );
				assert( instrRef );

				pNote = new Note( instrRef, nPosition, fVelocity, fPan, nLength, nPitch );
				pNote->set_lead_lag( fLeadLag );

				//infoLog( "new note!! pos: " + toString( pNote->m_nPosition ) + "\t instr: " + instrId );
				pPattern->insert_note( pNote );

				noteNode = ( QDomNode ) noteNode.nextSiblingElement( "note" );
			}
			sequenceNode = ( QDomNode ) sequenceNode.nextSiblingElement( "sequence" );
		}
	}

	return pPattern;
}
};
