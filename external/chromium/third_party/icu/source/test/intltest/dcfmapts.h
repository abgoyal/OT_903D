

#ifndef _INTLTESTDECIMALFORMATAPI
#define _INTLTESTDECIMALFORMATAPI

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING

#include "unicode/unistr.h"
#include "intltest.h"


class IntlTestDecimalFormatAPI: public IntlTest {
    void runIndexedTest( int32_t index, UBool exec, const char* &name, char* par = NULL );  

public:
    /**
     * Tests basic functionality of various API functions for DecimalFormat
     **/
    void testAPI(/*char *par*/);
    void testRounding(/*char *par*/);
    void testRoundingInc(/*char *par*/);
private:
    /*Helper functions */
    void verify(const UnicodeString& message, const UnicodeString& got, double expected);
};

#endif /* #if !UCONFIG_NO_FORMATTING */

#endif
