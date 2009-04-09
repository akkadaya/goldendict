/* This file is (c) 2008-2009 Konstantin Isakov <ikm@users.berlios.de>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "wordfinder.hh"
#include "folding.hh"
#include <QThreadPool>
#include <map>

using std::vector;
using std::list;
using std::wstring;
using std::map;
using std::pair;

WordFinder::WordFinder( QObject * parent ):
  QObject( parent ), searchInProgress( false ),
  updateResultsTimer( this ),
  searchQueued( false )
{
  updateResultsTimer.setInterval( 1000 ); // We use a one second update timer
  updateResultsTimer.setSingleShot( true );

  connect( &updateResultsTimer, SIGNAL( timeout() ),
           this, SLOT( updateResults() ) );
}

WordFinder::~WordFinder()
{
  clear();
}

void WordFinder::prefixMatch( QString const & str,
                              std::vector< sptr< Dictionary::Class > > const & dicts )
{
  cancel();

  searchQueued = true;
  inputWord = str;
  inputDicts = &dicts;

  resultsArray.clear();
  resultsIndex.clear();
  searchResults.clear();

  if ( queuedRequests.empty() )
  {
    // No requests are queued, no need to wait for them to finish.
    startSearch();
  }

  // Else some requests are still queued, last one to finish would trigger
  // new search. This shouldn't take a lot of time, since they were all
  // cancelled, but still it could take some time.
}

void WordFinder::startSearch()
{
  if ( !searchQueued )
    return; // Search was probably cancelled

  // Clear the requests just in case
  queuedRequests.clear();
  finishedRequests.clear();

  searchErrorString.clear();

  searchQueued = false;
  searchInProgress = true;

  wstring word = inputWord.toStdWString();
  
  for( size_t x = 0; x < inputDicts->size(); ++x )
  {
    sptr< Dictionary::WordSearchRequest > sr = (*inputDicts)[ x ]->prefixMatch( word, 40 );

    connect( sr.get(), SIGNAL( finished() ),
             this, SLOT( requestFinished() ), Qt::QueuedConnection );

    queuedRequests.push_back( sr );
  }

  // Handle any requests finished already

  requestFinished();
}

void WordFinder::cancel()
{
  searchQueued = false;
  searchInProgress = false;
  
  cancelSearches();
}

void WordFinder::clear()
{
  cancel();
  queuedRequests.clear();
  finishedRequests.clear();
}

void WordFinder::requestFinished()
{
  bool newResults = false;

  // See how many new requests have finished, and if we have any new results
  for( list< sptr< Dictionary::WordSearchRequest > >::iterator i =
         queuedRequests.begin(); i != queuedRequests.end(); )
  {
    if ( (*i)->isFinished() )
    {
      if ( searchInProgress && !(*i)->getErrorString().isEmpty() )
        searchErrorString = tr( "Failed to query some dictionaries." );

      if ( (*i)->matchesCount() )
      {
        newResults = true;

        // This list is handled by updateResults()
        finishedRequests.splice( finishedRequests.end(), queuedRequests, i++ );
      }
      else // We won't do anything with it anymore, so we erase it
        queuedRequests.erase( i++ );
    }
    else
      ++i;
  }

  if ( !searchInProgress )
  {
    // There is no search in progress, so we just wait until there's
    // no requests left
    
    if ( queuedRequests.empty() )
    {
      // We got rid of all queries, queued search can now start
      finishedRequests.clear();
  
      if ( searchQueued )
        startSearch();
    }

    return;
  }

  if ( newResults && queuedRequests.size() && !updateResultsTimer.isActive() )
  {
    // If we have got some new results, but not all of them, we would start a
    // timer to update a user some time in the future
    updateResultsTimer.start();
  }

  if ( queuedRequests.empty() )
  {
    // Search is finished.
    updateResults();
  }
}

namespace {

/// Checks whether the first string has the second one inside, surrounded from
/// both sides by either whitespace, punctuation or begin/end of string.
/// If true is returned, pos holds the offset in the haystack. If the offset
/// is larger than 255, it is set to 255.
bool hasSurroundedWithWs( wstring const & haystack, wstring const & needle,
                          wstring::size_type & pos )
{
  if ( haystack.size() < needle.size() )
    return false; // Needle won't even fit into a haystack

  for( pos = 0; ; ++pos )
  {
    pos = haystack.find( needle, pos );
  
    if ( pos == wstring::npos )
      return false; // Not found
  
    if ( ( !pos || Folding::isWhitespace( haystack[ pos - 1 ] ) ||
           Folding::isPunct( haystack[ pos - 1 ] ) ) &&
         ( ( pos + needle.size() == haystack.size() ) ||
           Folding::isWhitespace( haystack[ pos + needle.size() ] ) ||
           Folding::isPunct( haystack[ pos + needle.size() ] ) ) )
    {
      if ( pos > 255 )
        pos = 255;

      return true;
    }
  }
}

}

void WordFinder::updateResults()
{
  if ( !searchInProgress )
    return; // Old queued signal

  if ( updateResultsTimer.isActive() )
    updateResultsTimer.stop(); // Can happen when we were done before it'd expire

  for( list< sptr< Dictionary::WordSearchRequest > >::iterator i =
         finishedRequests.begin(); i != finishedRequests.end(); )
  {
    for( size_t count = (*i)->matchesCount(), x = 0; x < count; ++x )
    {
      wstring match = (**i)[ x ].word;
      int weight = (**i)[ x ].weight;
      wstring lowerCased = Folding::applySimpleCaseOnly( match );

      pair< ResultsIndex::iterator, bool > insertResult =
        resultsIndex.insert( pair< wstring, ResultsArray::iterator >( lowerCased,
                                                                      resultsArray.end() ) );

      if ( !insertResult.second )
      {
        // Wasn't inserted since there was already an item -- check the case
        if ( insertResult.first->second->word != match )
        {
          // The case is different -- agree on a lowercase version
          insertResult.first->second->word = lowerCased;
        }
        if ( !weight && insertResult.first->second->wasSuggested )
          insertResult.first->second->wasSuggested = false;
      }
      else
      {
        resultsArray.push_back( OneResult() );

        resultsArray.back().word = match;
        resultsArray.back().rank = -1;
        resultsArray.back().wasSuggested = ( weight != 0 );

        insertResult.first->second = --resultsArray.end();
      }
    }
    finishedRequests.erase( i++ );
  }

  if ( resultsArray.size() )
  {
    /// Assign each result a category, storing it in the rank's field

    enum Category
    {
      ExactMatch,
      ExactNoFullCaseMatch,
      ExactNoDiaMatch,
      ExactNoPunctMatch,
      ExactNoWsMatch,
      ExactInsideMatch,
      ExactNoDiaInsideMatch,
      ExactNoPunctInsideMatch,
      PrefixMatch,
      PrefixNoDiaMatch,
      PrefixNoPunctMatch,
      PrefixNoWsMatch,
      WorstMatch,
      Multiplier = 256 // Categories should be multiplied by Multiplier
    };

    wstring target = Folding::applySimpleCaseOnly( inputWord.toStdWString() );
    wstring targetNoFullCase = Folding::applyFullCaseOnly( target );
    wstring targetNoDia = Folding::applyDiacriticsOnly( targetNoFullCase );
    wstring targetNoPunct = Folding::applyPunctOnly( targetNoDia );
    wstring targetNoWs = Folding::applyWhitespaceOnly( targetNoPunct );

    wstring::size_type matchPos = 0;

    for( ResultsIndex::const_iterator i = resultsIndex.begin(), j = resultsIndex.end();
         i != j; ++i )
    {
      wstring resultNoFullCase, resultNoDia, resultNoPunct, resultNoWs;

      if ( i->first == target )
        i->second->rank = ExactMatch * Multiplier;
      else
      if ( ( resultNoFullCase = Folding::applyFullCaseOnly( i->first ) ) == targetNoFullCase )
        i->second->rank = ExactNoFullCaseMatch * Multiplier;
      else
      if ( ( resultNoDia = Folding::applyDiacriticsOnly( resultNoFullCase ) ) == targetNoDia )
        i->second->rank = ExactNoDiaMatch * Multiplier;
      else
      if ( ( resultNoPunct = Folding::applyPunctOnly( resultNoDia ) ) == targetNoPunct )
        i->second->rank = ExactNoPunctMatch * Multiplier;
      else
      if ( ( resultNoWs = Folding::applyWhitespaceOnly( resultNoPunct ) ) == targetNoWs )
        i->second->rank = ExactNoWsMatch * Multiplier;
      else
      if ( hasSurroundedWithWs( i->first, target, matchPos ) )
        i->second->rank = ExactInsideMatch * Multiplier + matchPos;
      else
      if ( hasSurroundedWithWs( resultNoDia, targetNoDia, matchPos ) )
        i->second->rank = ExactNoDiaInsideMatch * Multiplier + matchPos;
      else
      if ( hasSurroundedWithWs( resultNoPunct, targetNoPunct, matchPos ) )
        i->second->rank = ExactNoPunctInsideMatch * Multiplier + matchPos;
      else
      if ( i->first.size() > target.size() && i->first.compare( 0, target.size(), target ) == 0 )
        i->second->rank = PrefixMatch * Multiplier;
      else
      if ( resultNoDia.size() > targetNoDia.size() && resultNoDia.compare( 0, targetNoDia.size(), targetNoDia ) == 0 )
        i->second->rank = PrefixNoDiaMatch * Multiplier;
      else
      if ( resultNoPunct.size() > targetNoPunct.size() && resultNoPunct.compare( 0, targetNoPunct.size(), targetNoPunct ) == 0 )
        i->second->rank = PrefixNoPunctMatch * Multiplier;
      else
      if ( resultNoWs.size() > targetNoWs.size() && resultNoWs.compare( 0, targetNoWs.size(), targetNoWs ) == 0 )
        i->second->rank = PrefixNoWsMatch * Multiplier;
      else
        i->second->rank = WorstMatch * Multiplier;
    }

    resultsArray.sort( SortByRank() );
  }

  searchResults.clear();
  searchResults.reserve( resultsArray.size() < 500 ? resultsArray.size() : 500 );

  for( ResultsArray::const_iterator i = resultsArray.begin(), j = resultsArray.end();
       i != j; ++i )
  {
    //printf( "%d: %ls\n", i->second, i->first.c_str() );

    if ( searchResults.size() < 500 )
      searchResults.push_back( std::pair< QString, bool >( QString::fromStdWString( i->word ), i->wasSuggested ) );
    else
      break;
  }

  if ( queuedRequests.size() )
  {
    // There are still some unhandled results.
    emit updated();
  }
  else
  {
    // That were all of them.
    searchInProgress = false;
    emit finished();
  }
}

void WordFinder::cancelSearches()
{
  for( list< sptr< Dictionary::WordSearchRequest > >::iterator i =
         queuedRequests.begin(); i != queuedRequests.end(); ++i )
    (*i)->cancel();
}

