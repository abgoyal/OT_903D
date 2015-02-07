
#include "LETypes.h"
#include "LEFontInstance.h"
#include "OpenTypeTables.h"
#include "GlyphSubstitutionTables.h"
#include "GlyphIterator.h"
#include "LookupProcessor.h"
#include "SubstitutionLookups.h"
#include "CoverageTables.h"
#include "LESwaps.h"

U_NAMESPACE_BEGIN

void SubstitutionLookup::applySubstitutionLookups(
        LookupProcessor *lookupProcessor,
        SubstitutionLookupRecord *substLookupRecordArray,
        le_uint16 substCount,
        GlyphIterator *glyphIterator,
        const LEFontInstance *fontInstance,
        le_int32 position,
        LEErrorCode& success)
{
    if (LE_FAILURE(success)) { 
        return;
    }

    GlyphIterator tempIterator(*glyphIterator);

    for (le_uint16 subst = 0; subst < substCount && LE_SUCCESS(success); subst += 1) {
        le_uint16 sequenceIndex = SWAPW(substLookupRecordArray[subst].sequenceIndex);
        le_uint16 lookupListIndex = SWAPW(substLookupRecordArray[subst].lookupListIndex);

        tempIterator.setCurrStreamPosition(position);
        tempIterator.next(sequenceIndex);

        lookupProcessor->applySingleLookup(lookupListIndex, &tempIterator, fontInstance, success);
    }
}

U_NAMESPACE_END