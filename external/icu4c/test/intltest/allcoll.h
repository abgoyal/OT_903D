

#ifndef _ALLCOLL
#define _ALLCOLL

#include "unicode/utypes.h"

#if !UCONFIG_NO_COLLATION

#include "unicode/tblcoll.h"
#include "tscoll.h"

class CollationDummyTest: public IntlTestCollator {
public:
    // If this is too small for the test data, just increase it.
    // Just don't make it too large, otherwise the executable will get too big
    enum EToken_Len { MAX_TOKEN_LEN = 16 };

    CollationDummyTest();
    virtual ~CollationDummyTest();
    void runIndexedTest( int32_t index, UBool exec, const char* &name, char* /*par = NULL */);

    // perform test with strength PRIMARY
    void TestPrimary(/* char* par */);

    // perform test with strength SECONDARY
    void TestSecondary(/* char* par */);

    // perform test with strength tertiary
    void TestTertiary(/* char* par */);

    // perform extra tests
    void TestExtra(/* char* par */);

    void TestIdentical();

    void TestJB581();

private:
    static const  Collator::EComparisonResult results[];

    RuleBasedCollator *myCollation;
};

#endif /* #if !UCONFIG_NO_COLLATION */

#endif
