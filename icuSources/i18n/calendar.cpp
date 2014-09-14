/*
*******************************************************************************
* Copyright (C) 1997-2014, International Business Machines Corporation and    *
* others. All Rights Reserved.                                                *
*******************************************************************************
*
* File CALENDAR.CPP
*
* Modification History:
*
*   Date        Name        Description
*   02/03/97    clhuang     Creation.
*   04/22/97    aliu        Cleaned up, fixed memory leak, made 
*                           setWeekCountData() more robust.  
*                           Moved platform code to TPlatformUtilities.
*   05/01/97    aliu        Made equals(), before(), after() arguments const.
*   05/20/97    aliu        Changed logic of when to compute fields and time
*                           to fix bugs.
*   08/12/97    aliu        Added equivalentTo.  Misc other fixes.
*   07/28/98    stephen     Sync up with JDK 1.2
*   09/02/98    stephen     Sync with JDK 1.2 8/31 build (getActualMin/Max)
*   03/17/99    stephen     Changed adoptTimeZone() - now fAreFieldsSet is
*                           set to FALSE to force update of time.
*******************************************************************************
*/

#include "utypeinfo.h"  // for 'typeid' to work 

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING

#include "unicode/gregocal.h"
#include "unicode/basictz.h"
#include "unicode/simpletz.h"
#include "unicode/rbtz.h"
#include "unicode/vtzone.h"
#include "gregoimp.h"
#include "buddhcal.h"
#include "taiwncal.h"
#include "japancal.h"
#include "islamcal.h"
#include "hebrwcal.h"
#include "persncal.h"
#include "indiancal.h"
#include "chnsecal.h"
#include "coptccal.h"
#include "dangical.h"
#include "ethpccal.h"
#include "unicode/calendar.h"
#include "cpputils.h"
#include "servloc.h"
#include "ucln_in.h"
#include "cstring.h"
#include "locbased.h"
#include "uresimp.h"
#include "ustrenum.h"
#include "uassert.h"
#include "olsontz.h"

#if !UCONFIG_NO_SERVICE
static icu::ICULocaleService* gService = NULL;
#endif

// INTERNAL - for cleanup

U_CDECL_BEGIN
static UBool calendar_cleanup(void) {
#if !UCONFIG_NO_SERVICE
    if (gService) {
        delete gService;
        gService = NULL;
    }
#endif
    return TRUE;
}
U_CDECL_END

// ------------------------------------------
//
// Registration
//
//-------------------------------------------
//#define U_DEBUG_CALSVC 1
//

#if defined( U_DEBUG_CALSVC ) || defined (U_DEBUG_CAL)

/** 
 * fldName was removed as a duplicate implementation. 
 * use  udbg_ services instead, 
 * which depend on include files and library from ../tools/toolutil, the following circular link:
 *   CPPFLAGS+=-I$(top_srcdir)/tools/toolutil
 *   LIBS+=$(LIBICUTOOLUTIL)
 */
#include "udbgutil.h"
#include <stdio.h>

/**
* convert a UCalendarDateFields into a string - for debugging
* @param f field enum
* @return static string to the field name
* @internal
*/

const char* fldName(UCalendarDateFields f) {
	return udbg_enumName(UDBG_UCalendarDateFields, (int32_t)f);
}

#if UCAL_DEBUG_DUMP
// from CalendarTest::calToStr - but doesn't modify contents.
void ucal_dump(const Calendar &cal) {
    cal.dump();
}

void Calendar::dump() const {
    int i;
    fprintf(stderr, "@calendar=%s, timeset=%c, fieldset=%c, allfields=%c, virtualset=%c, t=%.2f",
        getType(), fIsTimeSet?'y':'n',  fAreFieldsSet?'y':'n',  fAreAllFieldsSet?'y':'n',  
        fAreFieldsVirtuallySet?'y':'n',
        fTime);

    // can add more things here: DST, zone, etc.
    fprintf(stderr, "\n");
    for(i = 0;i<UCAL_FIELD_COUNT;i++) {
        int n;
        const char *f = fldName((UCalendarDateFields)i);
        fprintf(stderr, "  %25s: %-11ld", f, fFields[i]);
        if(fStamp[i] == kUnset) {
            fprintf(stderr, " (unset) ");
        } else if(fStamp[i] == kInternallySet) { 
            fprintf(stderr, " (internally set) ");
            //} else if(fStamp[i] == kInternalDefault) { 
            //    fprintf(stderr, " (internal default) ");
        } else {
            fprintf(stderr, " %%%d ", fStamp[i]);
        }
        fprintf(stderr, "\n");

    }
}

U_CFUNC void ucal_dump(UCalendar* cal) {
    ucal_dump( *((Calendar*)cal)  );
}
#endif

#endif

/* Max value for stamp allowable before recalculation */
#define STAMP_MAX 10000

static const char * const gCalTypes[] = {
    "gregorian",
    "japanese",
    "buddhist",
    "roc",
    "persian",
    "islamic-civil",
    "islamic",
    "hebrew",
    "chinese",
    "indian",
    "coptic",
    "ethiopic",
    "ethiopic-amete-alem",
    "iso8601",
    "dangi",
    NULL
};

// Must be in the order of gCalTypes above
typedef enum ECalType {
    CALTYPE_UNKNOWN = -1,
    CALTYPE_GREGORIAN = 0,
    CALTYPE_JAPANESE,
    CALTYPE_BUDDHIST,
    CALTYPE_ROC,
    CALTYPE_PERSIAN,
    CALTYPE_ISLAMIC_CIVIL,
    CALTYPE_ISLAMIC,
    CALTYPE_HEBREW,
    CALTYPE_CHINESE,
    CALTYPE_INDIAN,
    CALTYPE_COPTIC,
    CALTYPE_ETHIOPIC,
    CALTYPE_ETHIOPIC_AMETE_ALEM,
    CALTYPE_ISO8601,
    CALTYPE_DANGI
} ECalType;

U_NAMESPACE_BEGIN

static ECalType getCalendarType(const char *s) {
    for (int i = 0; gCalTypes[i] != NULL; i++) {
        if (uprv_stricmp(s, gCalTypes[i]) == 0) {
            return (ECalType)i;
        }
    }
    return CALTYPE_UNKNOWN;
}

static UBool isStandardSupportedKeyword(const char *keyword, UErrorCode& status) { 
    if(U_FAILURE(status)) {
        return FALSE;
    }
    ECalType calType = getCalendarType(keyword);
    return (calType != CALTYPE_UNKNOWN);
}

static void getCalendarKeyword(const UnicodeString &id, char *targetBuffer, int32_t targetBufferSize) {
    UnicodeString calendarKeyword = UNICODE_STRING_SIMPLE("calendar=");
    int32_t calKeyLen = calendarKeyword.length();
    int32_t keyLen = 0;

    int32_t keywordIdx = id.indexOf((UChar)0x003D); /* '=' */
    if (id[0] == 0x40/*'@'*/
        && id.compareBetween(1, keywordIdx+1, calendarKeyword, 0, calKeyLen) == 0)
    {
        keyLen = id.extract(keywordIdx+1, id.length(), targetBuffer, targetBufferSize, US_INV);
    }
    targetBuffer[keyLen] = 0;
}

static ECalType getCalendarTypeForLocale(const char *locid) {
    UErrorCode status = U_ZERO_ERROR;
    ECalType calType = CALTYPE_UNKNOWN;

    //TODO: ULOC_FULL_NAME is out of date and too small..
    char canonicalName[256];

    // canonicalize, so grandfathered variant will be transformed to keywords
    // e.g ja_JP_TRADITIONAL -> ja_JP@calendar=japanese
    int32_t canonicalLen = uloc_canonicalize(locid, canonicalName, sizeof(canonicalName) - 1, &status);
    if (U_FAILURE(status)) {
        return CALTYPE_GREGORIAN;
    }
    canonicalName[canonicalLen] = 0;    // terminate

    char calTypeBuf[32];
    int32_t calTypeBufLen;

    calTypeBufLen = uloc_getKeywordValue(canonicalName, "calendar", calTypeBuf, sizeof(calTypeBuf) - 1, &status);
    if (U_SUCCESS(status)) {
        calTypeBuf[calTypeBufLen] = 0;
        calType = getCalendarType(calTypeBuf);
        if (calType != CALTYPE_UNKNOWN) {
            return calType;
        }
    }
    status = U_ZERO_ERROR;

    // when calendar keyword is not available or not supported, read supplementalData
    // to get the default calendar type for the locale's region
    char region[ULOC_COUNTRY_CAPACITY];
    int32_t regionLen = 0;
    regionLen = uloc_getCountry(canonicalName, region, sizeof(region) - 1, &status);
    if (regionLen == 0) {
        char fullLoc[256];
        uloc_addLikelySubtags(locid, fullLoc, sizeof(fullLoc) - 1, &status);
        regionLen = uloc_getCountry(fullLoc, region, sizeof(region) - 1, &status);
    }
    if (U_FAILURE(status)) {
        return CALTYPE_GREGORIAN;
    }
    region[regionLen] = 0;
    
    // Read preferred calendar values from supplementalData calendarPreference
    UResourceBundle *rb = ures_openDirect(NULL, "supplementalData", &status);
    ures_getByKey(rb, "calendarPreferenceData", rb, &status);
    UResourceBundle *order = ures_getByKey(rb, region, NULL, &status);
    if (status == U_MISSING_RESOURCE_ERROR && rb != NULL) {
        status = U_ZERO_ERROR;
        order = ures_getByKey(rb, "001", NULL, &status);
    }

    calTypeBuf[0] = 0;
    if (U_SUCCESS(status) && order != NULL) {
        // the first calender type is the default for the region
        int32_t len = 0;
        const UChar *uCalType = ures_getStringByIndex(order, 0, &len, &status);
        if (len < (int32_t)sizeof(calTypeBuf)) {
            u_UCharsToChars(uCalType, calTypeBuf, len);
            *(calTypeBuf + len) = 0; // terminate;
            calType = getCalendarType(calTypeBuf);
        }
    }

    ures_close(order);
    ures_close(rb);

    if (calType == CALTYPE_UNKNOWN) {
        // final fallback
        calType = CALTYPE_GREGORIAN;
    }
    return calType;
}

static Calendar *createStandardCalendar(ECalType calType, const Locale &loc, UErrorCode& status) {
    Calendar *cal = NULL;

    switch (calType) {
        case CALTYPE_GREGORIAN:
            cal = new GregorianCalendar(loc, status);
            break;
        case CALTYPE_JAPANESE:
            cal = new JapaneseCalendar(loc, status);
            break;
        case CALTYPE_BUDDHIST:
            cal = new BuddhistCalendar(loc, status);
            break;
        case CALTYPE_ROC:
            cal = new TaiwanCalendar(loc, status);
            break;
        case CALTYPE_PERSIAN:
            cal = new PersianCalendar(loc, status);
            break;
        case CALTYPE_ISLAMIC_CIVIL:
            cal = new IslamicCalendar(loc, status, IslamicCalendar::CIVIL);
            break;
        case CALTYPE_ISLAMIC:
            cal = new IslamicCalendar(loc, status, IslamicCalendar::ASTRONOMICAL);
            break;
        case CALTYPE_HEBREW:
            cal = new HebrewCalendar(loc, status);
            break;
        case CALTYPE_CHINESE:
            cal = new ChineseCalendar(loc, status);
            break;
        case CALTYPE_INDIAN:
            cal = new IndianCalendar(loc, status);
            break;
        case CALTYPE_COPTIC:
            cal = new CopticCalendar(loc, status);
            break;
        case CALTYPE_ETHIOPIC:
            cal = new EthiopicCalendar(loc, status, EthiopicCalendar::AMETE_MIHRET_ERA);
            break;
        case CALTYPE_ETHIOPIC_AMETE_ALEM:
            cal = new EthiopicCalendar(loc, status, EthiopicCalendar::AMETE_ALEM_ERA);
            break;
        case CALTYPE_ISO8601:
            cal = new GregorianCalendar(loc, status);
            cal->setFirstDayOfWeek(UCAL_MONDAY);
            cal->setMinimalDaysInFirstWeek(4);
            break;
        case CALTYPE_DANGI:
            cal = new DangiCalendar(loc, status);
            break;
        default:
            status = U_UNSUPPORTED_ERROR;
    }
    return cal;
}


#if !UCONFIG_NO_SERVICE

// -------------------------------------

/**
* a Calendar Factory which creates the "basic" calendar types, that is, those 
* shipped with ICU.
*/
class BasicCalendarFactory : public LocaleKeyFactory {
public:
    /**
    * @param calendarType static const string (caller owns storage - will be aliased) to calendar type
    */
    BasicCalendarFactory()
        : LocaleKeyFactory(LocaleKeyFactory::INVISIBLE) { }

    virtual ~BasicCalendarFactory();

protected:
    //virtual UBool isSupportedID( const UnicodeString& id, UErrorCode& status) const { 
    //  if(U_FAILURE(status)) {
    //    return FALSE;
    //  }
    //  char keyword[ULOC_FULLNAME_CAPACITY];
    //  getCalendarKeyword(id, keyword, (int32_t)sizeof(keyword));
    //  return isStandardSupportedKeyword(keyword, status);
    //}

    virtual void updateVisibleIDs(Hashtable& result, UErrorCode& status) const
    {
        if (U_SUCCESS(status)) {
            for(int32_t i=0;gCalTypes[i] != NULL;i++) {
                UnicodeString id((UChar)0x40); /* '@' a variant character */
                id.append(UNICODE_STRING_SIMPLE("calendar="));
                id.append(UnicodeString(gCalTypes[i], -1, US_INV));
                result.put(id, (void*)this, status);
            }
        }
    }

    virtual UObject* create(const ICUServiceKey& key, const ICUService* /*service*/, UErrorCode& status) const {
#ifdef U_DEBUG_CALSVC
        if(dynamic_cast<const LocaleKey*>(&key) == NULL) {
            fprintf(stderr, "::create - not a LocaleKey!\n");
        }
#endif
        const LocaleKey& lkey = (LocaleKey&)key;
        Locale curLoc;  // current locale
        Locale canLoc;  // Canonical locale

        lkey.currentLocale(curLoc);
        lkey.canonicalLocale(canLoc);

        char keyword[ULOC_FULLNAME_CAPACITY];
        UnicodeString str;

        key.currentID(str);
        getCalendarKeyword(str, keyword, (int32_t) sizeof(keyword));

#ifdef U_DEBUG_CALSVC
        fprintf(stderr, "BasicCalendarFactory::create() - cur %s, can %s\n", (const char*)curLoc.getName(), (const char*)canLoc.getName());
#endif

        if(!isStandardSupportedKeyword(keyword,status)) {  // Do we handle this type?
#ifdef U_DEBUG_CALSVC

            fprintf(stderr, "BasicCalendarFactory - not handling %s.[%s]\n", (const char*) curLoc.getName(), tmp );
#endif
            return NULL;
        }

        return createStandardCalendar(getCalendarType(keyword), canLoc, status);
    }
};

BasicCalendarFactory::~BasicCalendarFactory() {}

/** 
* A factory which looks up the DefaultCalendar resource to determine which class of calendar to use
*/

class DefaultCalendarFactory : public ICUResourceBundleFactory {
public:
    DefaultCalendarFactory() : ICUResourceBundleFactory() { }
    virtual ~DefaultCalendarFactory();
protected:
    virtual UObject* create(const ICUServiceKey& key, const ICUService* /*service*/, UErrorCode& status) const  {

        LocaleKey &lkey = (LocaleKey&)key;
        Locale loc;
        lkey.currentLocale(loc);

        UnicodeString *ret = new UnicodeString();
        if (ret == NULL) {
            status = U_MEMORY_ALLOCATION_ERROR;
        } else {
            ret->append((UChar)0x40); // '@' is a variant character
            ret->append(UNICODE_STRING("calendar=", 9));
            ret->append(UnicodeString(gCalTypes[getCalendarTypeForLocale(loc.getName())], -1, US_INV));
        }
        return ret;
    }
};

DefaultCalendarFactory::~DefaultCalendarFactory() {}

// -------------------------------------
class CalendarService : public ICULocaleService {
public:
    CalendarService()
        : ICULocaleService(UNICODE_STRING_SIMPLE("Calendar"))
    {
        UErrorCode status = U_ZERO_ERROR;
        registerFactory(new DefaultCalendarFactory(), status);
    }

    virtual ~CalendarService();

    virtual UObject* cloneInstance(UObject* instance) const {
        UnicodeString *s = dynamic_cast<UnicodeString *>(instance);
        if(s != NULL) {
            return s->clone(); 
        } else {
#ifdef U_DEBUG_CALSVC_F
            UErrorCode status2 = U_ZERO_ERROR;
            fprintf(stderr, "Cloning a %s calendar with tz=%ld\n", ((Calendar*)instance)->getType(), ((Calendar*)instance)->get(UCAL_ZONE_OFFSET, status2));
#endif
            return ((Calendar*)instance)->clone();
        }
    }

    virtual UObject* handleDefault(const ICUServiceKey& key, UnicodeString* /*actualID*/, UErrorCode& status) const {
        LocaleKey& lkey = (LocaleKey&)key;
        //int32_t kind = lkey.kind();

        Locale loc;
        lkey.canonicalLocale(loc);

#ifdef U_DEBUG_CALSVC
        Locale loc2;
        lkey.currentLocale(loc2);
        fprintf(stderr, "CalSvc:handleDefault for currentLoc %s, canloc %s\n", (const char*)loc.getName(),  (const char*)loc2.getName());
#endif
        Calendar *nc =  new GregorianCalendar(loc, status);

#ifdef U_DEBUG_CALSVC
        UErrorCode status2 = U_ZERO_ERROR;
        fprintf(stderr, "New default calendar has tz=%d\n", ((Calendar*)nc)->get(UCAL_ZONE_OFFSET, status2));
#endif
        return nc;
    }

    virtual UBool isDefault() const {
        return countFactories() == 1;
    }
};

CalendarService::~CalendarService() {}

// -------------------------------------

static inline UBool
isCalendarServiceUsed() {
    UBool retVal;
    UMTX_CHECK(NULL, gService != NULL, retVal);
    return retVal;
}

// -------------------------------------

static ICULocaleService* 
getCalendarService(UErrorCode &status)
{
    UBool needInit;
    UMTX_CHECK(NULL, (UBool)(gService == NULL), needInit);
    if (needInit) {
#ifdef U_DEBUG_CALSVC
        fprintf(stderr, "Spinning up Calendar Service\n");
#endif
        ICULocaleService * newservice = new CalendarService();
        if (newservice == NULL) {
            status = U_MEMORY_ALLOCATION_ERROR;
            return newservice;
        }
#ifdef U_DEBUG_CALSVC
        fprintf(stderr, "Registering classes..\n");
#endif

        // Register all basic instances. 
        newservice->registerFactory(new BasicCalendarFactory(),status);

#ifdef U_DEBUG_CALSVC
        fprintf(stderr, "Done..\n");
#endif

        if(U_FAILURE(status)) {
#ifdef U_DEBUG_CALSVC
            fprintf(stderr, "err (%s) registering classes, deleting service.....\n", u_errorName(status));
#endif
            delete newservice;
            newservice = NULL;
        }

        if (newservice) {
            umtx_lock(NULL);
            if (gService == NULL) {
                gService = newservice;
                newservice = NULL;
            }
            umtx_unlock(NULL);
        }
        if (newservice) {
            delete newservice;
        } else {
            // we won the contention - we can register the cleanup.
            ucln_i18n_registerCleanup(UCLN_I18N_CALENDAR, calendar_cleanup);
        }
    }
    return gService;
}

URegistryKey Calendar::registerFactory(ICUServiceFactory* toAdopt, UErrorCode& status)
{
    return getCalendarService(status)->registerFactory(toAdopt, status);
}

UBool Calendar::unregister(URegistryKey key, UErrorCode& status) {
    return getCalendarService(status)->unregister(key, status);
}
#endif /* UCONFIG_NO_SERVICE */

// -------------------------------------

static const int32_t kCalendarLimits[UCAL_FIELD_COUNT][4] = {
    //    Minimum  Greatest min      Least max   Greatest max
    {/*N/A*/-1,       /*N/A*/-1,     /*N/A*/-1,       /*N/A*/-1}, // ERA
    {/*N/A*/-1,       /*N/A*/-1,     /*N/A*/-1,       /*N/A*/-1}, // YEAR
    {/*N/A*/-1,       /*N/A*/-1,     /*N/A*/-1,       /*N/A*/-1}, // MONTH
    {/*N/A*/-1,       /*N/A*/-1,     /*N/A*/-1,       /*N/A*/-1}, // WEEK_OF_YEAR
    {/*N/A*/-1,       /*N/A*/-1,     /*N/A*/-1,       /*N/A*/-1}, // WEEK_OF_MONTH
    {/*N/A*/-1,       /*N/A*/-1,     /*N/A*/-1,       /*N/A*/-1}, // DAY_OF_MONTH
    {/*N/A*/-1,       /*N/A*/-1,     /*N/A*/-1,       /*N/A*/-1}, // DAY_OF_YEAR
    {           1,            1,             7,             7  }, // DAY_OF_WEEK
    {/*N/A*/-1,       /*N/A*/-1,     /*N/A*/-1,       /*N/A*/-1}, // DAY_OF_WEEK_IN_MONTH
    {           0,            0,             1,             1  }, // AM_PM
    {           0,            0,            11,            11  }, // HOUR
    {           0,            0,            23,            23  }, // HOUR_OF_DAY
    {           0,            0,            59,            59  }, // MINUTE
    {           0,            0,            59,            59  }, // SECOND
    {           0,            0,           999,           999  }, // MILLISECOND
    {-12*kOneHour, -12*kOneHour,   12*kOneHour,   15*kOneHour  }, // ZONE_OFFSET
    {           0,            0,    1*kOneHour,    1*kOneHour  }, // DST_OFFSET
    {/*N/A*/-1,       /*N/A*/-1,     /*N/A*/-1,       /*N/A*/-1}, // YEAR_WOY
    {           1,            1,             7,             7  }, // DOW_LOCAL
    {/*N/A*/-1,       /*N/A*/-1,     /*N/A*/-1,       /*N/A*/-1}, // EXTENDED_YEAR
    { -0x7F000000,  -0x7F000000,    0x7F000000,    0x7F000000  }, // JULIAN_DAY
    {           0,            0, 24*kOneHour-1, 24*kOneHour-1  },  // MILLISECONDS_IN_DAY
    {           0,            0,             1,             1  },  // IS_LEAP_MONTH
};

// Resource bundle tags read by this class
static const char gMonthNames[] = "monthNames";

// Data flow in Calendar
// ---------------------

// The current time is represented in two ways by Calendar: as UTC
// milliseconds from the epoch start (1 January 1970 0:00 UTC), and as local
// fields such as MONTH, HOUR, AM_PM, etc.  It is possible to compute the
// millis from the fields, and vice versa.  The data needed to do this
// conversion is encapsulated by a TimeZone object owned by the Calendar.
// The data provided by the TimeZone object may also be overridden if the
// user sets the ZONE_OFFSET and/or DST_OFFSET fields directly. The class
// keeps track of what information was most recently set by the caller, and
// uses that to compute any other information as needed.

// If the user sets the fields using set(), the data flow is as follows.
// This is implemented by the Calendar subclass's computeTime() method.
// During this process, certain fields may be ignored.  The disambiguation
// algorithm for resolving which fields to pay attention to is described
// above.

//   local fields (YEAR, MONTH, DATE, HOUR, MINUTE, etc.)
//           |
//           | Using Calendar-specific algorithm
//           V
//   local standard millis
//           |
//           | Using TimeZone or user-set ZONE_OFFSET / DST_OFFSET
//           V
//   UTC millis (in time data member)

// If the user sets the UTC millis using setTime(), the data flow is as
// follows.  This is implemented by the Calendar subclass's computeFields()
// method.

//   UTC millis (in time data member)
//           |
//           | Using TimeZone getOffset()
//           V
//   local standard millis
//           |
//           | Using Calendar-specific algorithm
//           V
//   local fields (YEAR, MONTH, DATE, HOUR, MINUTE, etc.)

// In general, a round trip from fields, through local and UTC millis, and
// back out to fields is made when necessary.  This is implemented by the
// complete() method.  Resolving a partial set of fields into a UTC millis
// value allows all remaining fields to be generated from that value.  If
// the Calendar is lenient, the fields are also renormalized to standard
// ranges when they are regenerated.

// -------------------------------------

Calendar::Calendar(UErrorCode& success)
:   UObject(),
fIsTimeSet(FALSE),
fAreFieldsSet(FALSE),
fAreAllFieldsSet(FALSE),
fAreFieldsVirtuallySet(FALSE),
fNextStamp((int32_t)kMinimumUserStamp),
fTime(0),
fLenient(TRUE),
fZone(0),
fRepeatedWallTime(UCAL_WALLTIME_LAST),
fSkippedWallTime(UCAL_WALLTIME_LAST)
{
    clear();
    fZone = TimeZone::createDefault();
    if (fZone == NULL) {
        success = U_MEMORY_ALLOCATION_ERROR;
    }
    setWeekData(Locale::getDefault(), NULL, success);
}

// -------------------------------------

Calendar::Calendar(TimeZone* zone, const Locale& aLocale, UErrorCode& success)
:   UObject(),
fIsTimeSet(FALSE),
fAreFieldsSet(FALSE),
fAreAllFieldsSet(FALSE),
fAreFieldsVirtuallySet(FALSE),
fNextStamp((int32_t)kMinimumUserStamp),
fTime(0),
fLenient(TRUE),
fZone(0),
fRepeatedWallTime(UCAL_WALLTIME_LAST),
fSkippedWallTime(UCAL_WALLTIME_LAST)
{
    if(zone == 0) {
#if defined (U_DEBUG_CAL)
        fprintf(stderr, "%s:%d: ILLEGAL ARG because timezone cannot be 0\n",
            __FILE__, __LINE__);
#endif
        success = U_ILLEGAL_ARGUMENT_ERROR;
        return;
    }

    clear();    
    fZone = zone;

    setWeekData(aLocale, NULL, success);
}

// -------------------------------------

Calendar::Calendar(const TimeZone& zone, const Locale& aLocale, UErrorCode& success)
:   UObject(),
fIsTimeSet(FALSE),
fAreFieldsSet(FALSE),
fAreAllFieldsSet(FALSE),
fAreFieldsVirtuallySet(FALSE),
fNextStamp((int32_t)kMinimumUserStamp),
fTime(0),
fLenient(TRUE),
fZone(0),
fRepeatedWallTime(UCAL_WALLTIME_LAST),
fSkippedWallTime(UCAL_WALLTIME_LAST)
{
    clear();
    fZone = zone.clone();
    if (fZone == NULL) {
    	success = U_MEMORY_ALLOCATION_ERROR;
    }
    setWeekData(aLocale, NULL, success);
}

// -------------------------------------

Calendar::~Calendar()
{
    delete fZone;
}

// -------------------------------------

Calendar::Calendar(const Calendar &source)
:   UObject(source)
{
    fZone = 0;
    *this = source;
}

// -------------------------------------

Calendar &
Calendar::operator=(const Calendar &right)
{
    if (this != &right) {
        uprv_arrayCopy(right.fFields, fFields, UCAL_FIELD_COUNT);
        uprv_arrayCopy(right.fIsSet, fIsSet, UCAL_FIELD_COUNT);
        uprv_arrayCopy(right.fStamp, fStamp, UCAL_FIELD_COUNT);
        fTime                    = right.fTime;
        fIsTimeSet               = right.fIsTimeSet;
        fAreAllFieldsSet         = right.fAreAllFieldsSet;
        fAreFieldsSet            = right.fAreFieldsSet;
        fAreFieldsVirtuallySet   = right.fAreFieldsVirtuallySet;
        fLenient                 = right.fLenient;
        fRepeatedWallTime        = right.fRepeatedWallTime;
        fSkippedWallTime         = right.fSkippedWallTime;
        if (fZone != NULL) {
            delete fZone;
        }
        if (right.fZone != NULL) {
            fZone                = right.fZone->clone();
        }
        fFirstDayOfWeek          = right.fFirstDayOfWeek;
        fMinimalDaysInFirstWeek  = right.fMinimalDaysInFirstWeek;
        fWeekendOnset            = right.fWeekendOnset;
        fWeekendOnsetMillis      = right.fWeekendOnsetMillis;
        fWeekendCease            = right.fWeekendCease;
        fWeekendCeaseMillis      = right.fWeekendCeaseMillis;
        fNextStamp               = right.fNextStamp;
        uprv_strcpy(validLocale, right.validLocale);
        uprv_strcpy(actualLocale, right.actualLocale);
    }

    return *this;
}

// -------------------------------------

Calendar* U_EXPORT2
Calendar::createInstance(UErrorCode& success)
{
    return createInstance(TimeZone::createDefault(), Locale::getDefault(), success);
}

// -------------------------------------

Calendar* U_EXPORT2
Calendar::createInstance(const TimeZone& zone, UErrorCode& success)
{
    return createInstance(zone, Locale::getDefault(), success);
}

// -------------------------------------

Calendar* U_EXPORT2
Calendar::createInstance(const Locale& aLocale, UErrorCode& success)
{
    return createInstance(TimeZone::createDefault(), aLocale, success);
}

// ------------------------------------- Adopting 

// Note: this is the bottleneck that actually calls the service routines.

Calendar* U_EXPORT2
Calendar::createInstance(TimeZone* zone, const Locale& aLocale, UErrorCode& success)
{
    if (U_FAILURE(success)) {
        return NULL;
    }

    Locale actualLoc;
    UObject* u = NULL;

#if !UCONFIG_NO_SERVICE
    if (isCalendarServiceUsed()) {
        u = getCalendarService(success)->get(aLocale, LocaleKey::KIND_ANY, &actualLoc, success);
    }
    else
#endif
    {
        u = createStandardCalendar(getCalendarTypeForLocale(aLocale.getName()), aLocale, success);
    }
    Calendar* c = NULL;

    if(U_FAILURE(success) || !u) {
        delete zone;
        if(U_SUCCESS(success)) { // Propagate some kind of err
            success = U_INTERNAL_PROGRAM_ERROR;
        }
        return NULL;
    }

#if !UCONFIG_NO_SERVICE
    const UnicodeString* str = dynamic_cast<const UnicodeString*>(u);
    if(str != NULL) {
        // It's a unicode string telling us what type of calendar to load ("gregorian", etc)
        // Create a Locale over this string
        Locale l("");
        LocaleUtility::initLocaleFromName(*str, l);

#ifdef U_DEBUG_CALSVC
        fprintf(stderr, "Calendar::createInstance(%s), looking up [%s]\n", aLocale.getName(), l.getName());
#endif

        Locale actualLoc2;
        delete u;
        u = NULL;

        // Don't overwrite actualLoc, since the actual loc from this call
        // may be something like "@calendar=gregorian" -- TODO investigate
        // further...
        c = (Calendar*)getCalendarService(success)->get(l, LocaleKey::KIND_ANY, &actualLoc2, success);

        if(U_FAILURE(success) || !c) {
            delete zone;
            if(U_SUCCESS(success)) { 
                success = U_INTERNAL_PROGRAM_ERROR; // Propagate some err
            }
            return NULL;
        }

        str = dynamic_cast<const UnicodeString*>(c);
        if(str != NULL) {
            // recursed! Second lookup returned a UnicodeString. 
            // Perhaps DefaultCalendar{} was set to another locale.
#ifdef U_DEBUG_CALSVC
            char tmp[200];
            // Extract a char* out of it..
            int32_t len = str->length();
            int32_t actLen = sizeof(tmp)-1;
            if(len > actLen) {
                len = actLen;
            }
            str->extract(0,len,tmp);
            tmp[len]=0;

            fprintf(stderr, "err - recursed, 2nd lookup was unistring %s\n", tmp);
#endif
            success = U_MISSING_RESOURCE_ERROR;  // requested a calendar type which could NOT be found.
            delete c;
            delete zone;
            return NULL;
        }
#ifdef U_DEBUG_CALSVC
        fprintf(stderr, "%p: setting week count data to locale %s, actual locale %s\n", c, (const char*)aLocale.getName(), (const char *)actualLoc.getName());
#endif
        c->setWeekData(aLocale, c->getType(), success);  // set the correct locale (this was an indirected calendar)

        char keyword[ULOC_FULLNAME_CAPACITY];
        UErrorCode tmpStatus = U_ZERO_ERROR;
        l.getKeywordValue("calendar", keyword, ULOC_FULLNAME_CAPACITY, tmpStatus);
        if (U_SUCCESS(tmpStatus) && uprv_strcmp(keyword, "iso8601") == 0) {
            c->setFirstDayOfWeek(UCAL_MONDAY);
            c->setMinimalDaysInFirstWeek(4);
        }
    }
    else
#endif /* UCONFIG_NO_SERVICE */
    {
        // a calendar was returned - we assume the factory did the right thing.
        c = (Calendar*)u;
    }

    // Now, reset calendar to default state:
    c->adoptTimeZone(zone); //  Set the correct time zone
    c->setTimeInMillis(getNow(), success); // let the new calendar have the current time.

    return c;
}

// -------------------------------------

Calendar* U_EXPORT2
Calendar::createInstance(const TimeZone& zone, const Locale& aLocale, UErrorCode& success)
{
    Calendar* c = createInstance(aLocale, success);
    if(U_SUCCESS(success) && c) {
        c->setTimeZone(zone);
    }
    return c; 
}

// -------------------------------------

UBool
Calendar::operator==(const Calendar& that) const
{
    UErrorCode status = U_ZERO_ERROR;
    return isEquivalentTo(that) &&
        getTimeInMillis(status) == that.getTimeInMillis(status) &&
        U_SUCCESS(status);
}

UBool 
Calendar::isEquivalentTo(const Calendar& other) const
{
    return typeid(*this) == typeid(other) &&
        fLenient                == other.fLenient &&
        fRepeatedWallTime       == other.fRepeatedWallTime &&
        fSkippedWallTime        == other.fSkippedWallTime &&
        fFirstDayOfWeek         == other.fFirstDayOfWeek &&
        fMinimalDaysInFirstWeek == other.fMinimalDaysInFirstWeek &&
        fWeekendOnset           == other.fWeekendOnset &&
        fWeekendOnsetMillis     == other.fWeekendOnsetMillis &&
        fWeekendCease           == other.fWeekendCease &&
        fWeekendCeaseMillis     == other.fWeekendCeaseMillis &&
        *fZone                  == *other.fZone;
}

// -------------------------------------

UBool
Calendar::equals(const Calendar& when, UErrorCode& status) const
{
    return (this == &when ||
        getTime(status) == when.getTime(status));
}

// -------------------------------------

UBool
Calendar::before(const Calendar& when, UErrorCode& status) const
{
    return (this != &when &&
        getTimeInMillis(status) < when.getTimeInMillis(status));
}

// -------------------------------------

UBool
Calendar::after(const Calendar& when, UErrorCode& status) const
{
    return (this != &when &&
        getTimeInMillis(status) > when.getTimeInMillis(status));
}

// -------------------------------------


const Locale* U_EXPORT2
Calendar::getAvailableLocales(int32_t& count)
{
    return Locale::getAvailableLocales(count);
}

// -------------------------------------

StringEnumeration* U_EXPORT2
Calendar::getKeywordValuesForLocale(const char* key,
                    const Locale& locale, UBool commonlyUsed, UErrorCode& status)
{
    // This is a wrapper over ucal_getKeywordValuesForLocale
    UEnumeration *uenum = ucal_getKeywordValuesForLocale(key, locale.getName(),
                                                        commonlyUsed, &status);
    if (U_FAILURE(status)) {
        uenum_close(uenum);
        return NULL;
    }
    return new UStringEnumeration(uenum);
}

// -------------------------------------

UDate U_EXPORT2
Calendar::getNow()
{
    return uprv_getUTCtime(); // return as milliseconds
}

// -------------------------------------

/**
* Gets this Calendar's current time as a long.
* @return the current time as UTC milliseconds from the epoch.
*/
double 
Calendar::getTimeInMillis(UErrorCode& status) const
{
    if(U_FAILURE(status)) 
        return 0.0;

    if ( ! fIsTimeSet) 
        ((Calendar*)this)->updateTime(status);

    /* Test for buffer overflows */
    if(U_FAILURE(status)) {
        return 0.0;
    }
    return fTime;
}

// -------------------------------------

/**
* Sets this Calendar's current time from the given long value.
* A status of U_ILLEGAL_ARGUMENT_ERROR is set when millis is
* outside the range permitted by a Calendar object when not in lenient mode.
* when in lenient mode the out of range values are pinned to their respective min/max.
* @param date the new time in UTC milliseconds from the epoch.
*/
void 
Calendar::setTimeInMillis( double millis, UErrorCode& status ) {
    if(U_FAILURE(status)) 
        return;

    if (millis > MAX_MILLIS) {
        if(isLenient()) {
            millis = MAX_MILLIS;
        } else {
            status = U_ILLEGAL_ARGUMENT_ERROR;
            return;
        }
    } else if (millis < MIN_MILLIS) {
        if(isLenient()) {
            millis = MIN_MILLIS;
        } else {
            status = U_ILLEGAL_ARGUMENT_ERROR;
            return;
        }
    }

    fTime = millis;
    fAreFieldsSet = fAreAllFieldsSet = FALSE;
    fIsTimeSet = fAreFieldsVirtuallySet = TRUE;

    for (int32_t i=0; i<UCAL_FIELD_COUNT; ++i) {
        fFields[i]     = 0;
        fStamp[i]     = kUnset;
        fIsSet[i]     = FALSE;
    }
    

}

// -------------------------------------

int32_t
Calendar::get(UCalendarDateFields field, UErrorCode& status) const
{
    // field values are only computed when actually requested; for more on when computation
    // of various things happens, see the "data flow in Calendar" description at the top
    // of this file
    if (U_SUCCESS(status)) ((Calendar*)this)->complete(status); // Cast away const
    return U_SUCCESS(status) ? fFields[field] : 0;
}

// -------------------------------------

void
Calendar::set(UCalendarDateFields field, int32_t value)
{
    if (fAreFieldsVirtuallySet) {
        UErrorCode ec = U_ZERO_ERROR;
        computeFields(ec);
    }
    fFields[field]     = value;
    /* Ensure that the fNextStamp value doesn't go pass max value for int32_t */
    if (fNextStamp == STAMP_MAX) {
        recalculateStamp();
    }
    fStamp[field]     = fNextStamp++;
    fIsSet[field]     = TRUE; // Remove later
    fIsTimeSet = fAreFieldsSet = fAreFieldsVirtuallySet = FALSE;
}

// -------------------------------------

void
Calendar::set(int32_t year, int32_t month, int32_t date)
{
    set(UCAL_YEAR, year);
    set(UCAL_MONTH, month);
    set(UCAL_DATE, date);
}

// -------------------------------------

void
Calendar::set(int32_t year, int32_t month, int32_t date, int32_t hour, int32_t minute)
{
    set(UCAL_YEAR, year);
    set(UCAL_MONTH, month);
    set(UCAL_DATE, date);
    set(UCAL_HOUR_OF_DAY, hour);
    set(UCAL_MINUTE, minute);
}

// -------------------------------------

void
Calendar::set(int32_t year, int32_t month, int32_t date, int32_t hour, int32_t minute, int32_t second)
{
    set(UCAL_YEAR, year);
    set(UCAL_MONTH, month);
    set(UCAL_DATE, date);
    set(UCAL_HOUR_OF_DAY, hour);
    set(UCAL_MINUTE, minute);
    set(UCAL_SECOND, second);
}

// -------------------------------------

void
Calendar::clear()
{
    for (int32_t i=0; i<UCAL_FIELD_COUNT; ++i) {
        fFields[i]     = 0; // Must do this; other code depends on it
        fStamp[i]     = kUnset;
        fIsSet[i]     = FALSE; // Remove later
    }
    fIsTimeSet = fAreFieldsSet = fAreAllFieldsSet = fAreFieldsVirtuallySet = FALSE;
    // fTime is not 'cleared' - may be used if no fields are set.
}

// -------------------------------------

void
Calendar::clear(UCalendarDateFields field)
{
    if (fAreFieldsVirtuallySet) {
        UErrorCode ec = U_ZERO_ERROR;
        computeFields(ec);
    }
    fFields[field]         = 0;
    fStamp[field]         = kUnset;
    fIsSet[field]         = FALSE; // Remove later
    fIsTimeSet = fAreFieldsSet = fAreAllFieldsSet = fAreFieldsVirtuallySet = FALSE;
}

// -------------------------------------

UBool
Calendar::isSet(UCalendarDateFields field) const
{
    return fAreFieldsVirtuallySet || (fStamp[field] != kUnset);
}


int32_t Calendar::newestStamp(UCalendarDateFields first, UCalendarDateFields last, int32_t bestStampSoFar) const
{
    int32_t bestStamp = bestStampSoFar;
    for (int32_t i=(int32_t)first; i<=(int32_t)last; ++i) {
        if (fStamp[i] > bestStamp) {
            bestStamp = fStamp[i];
        }
    }
    return bestStamp;
}


// -------------------------------------

void
Calendar::complete(UErrorCode& status)
{
    if (!fIsTimeSet) {
        updateTime(status);
        /* Test for buffer overflows */
        if(U_FAILURE(status)) {
            return;
        }
    }
    if (!fAreFieldsSet) {
        computeFields(status); // fills in unset fields
        /* Test for buffer overflows */
        if(U_FAILURE(status)) {
            return;
        }
        fAreFieldsSet         = TRUE;
        fAreAllFieldsSet     = TRUE;
    }
}

//-------------------------------------------------------------------------
// Protected utility methods for use by subclasses.  These are very handy
// for implementing add, roll, and computeFields.
//-------------------------------------------------------------------------

/**
* Adjust the specified field so that it is within
* the allowable range for the date to which this calendar is set.
* For example, in a Gregorian calendar pinning the {@link #DAY_OF_MONTH DAY_OF_MONTH}
* field for a calendar set to April 31 would cause it to be set
* to April 30.
* <p>
* <b>Subclassing:</b>
* <br>
* This utility method is intended for use by subclasses that need to implement
* their own overrides of {@link #roll roll} and {@link #add add}.
* <p>
* <b>Note:</b>
* <code>pinField</code> is implemented in terms of
* {@link #getActualMinimum getActualMinimum}
* and {@link #getActualMaximum getActualMaximum}.  If either of those methods uses
* a slow, iterative algorithm for a particular field, it would be
* unwise to attempt to call <code>pinField</code> for that field.  If you
* really do need to do so, you should override this method to do
* something more efficient for that field.
* <p>
* @param field The calendar field whose value should be pinned.
*
* @see #getActualMinimum
* @see #getActualMaximum
* @stable ICU 2.0
*/
void Calendar::pinField(UCalendarDateFields field, UErrorCode& status) {
    int32_t max = getActualMaximum(field, status);
    int32_t min = getActualMinimum(field, status);

    if (fFields[field] > max) {
        set(field, max);
    } else if (fFields[field] < min) {
        set(field, min);
    }
}


void Calendar::computeFields(UErrorCode &ec)
{
  if (U_FAILURE(ec)) {
        return;
    }
    // Compute local wall millis
    double localMillis = internalGetTime();
    int32_t rawOffset, dstOffset;
    getTimeZone().getOffset(localMillis, FALSE, rawOffset, dstOffset, ec);
    localMillis += (rawOffset + dstOffset); 

    // Mark fields as set.  Do this before calling handleComputeFields().
    uint32_t mask =   //fInternalSetMask;
        (1 << UCAL_ERA) |
        (1 << UCAL_YEAR) |
        (1 << UCAL_MONTH) |
        (1 << UCAL_DAY_OF_MONTH) | // = UCAL_DATE
        (1 << UCAL_DAY_OF_YEAR) |
        (1 << UCAL_EXTENDED_YEAR);  

    for (int32_t i=0; i<UCAL_FIELD_COUNT; ++i) {
        if ((mask & 1) == 0) {
            fStamp[i] = kInternallySet;
            fIsSet[i] = TRUE; // Remove later
        } else {
            fStamp[i] = kUnset;
            fIsSet[i] = FALSE; // Remove later
        }
        mask >>= 1;
    }

    // We used to check for and correct extreme millis values (near
    // Long.MIN_VALUE or Long.MAX_VALUE) here.  Such values would cause
    // overflows from positive to negative (or vice versa) and had to
    // be manually tweaked.  We no longer need to do this because we
    // have limited the range of supported dates to those that have a
    // Julian day that fits into an int.  This allows us to implement a
    // JULIAN_DAY field and also removes some inelegant code. - Liu
    // 11/6/00

    int32_t days =  (int32_t)ClockMath::floorDivide(localMillis, (double)kOneDay);

    internalSet(UCAL_JULIAN_DAY,days + kEpochStartAsJulianDay);

#if defined (U_DEBUG_CAL)
    //fprintf(stderr, "%s:%d- Hmm! Jules @ %d, as per %.0lf millis\n",
    //__FILE__, __LINE__, fFields[UCAL_JULIAN_DAY], localMillis);
#endif  

    computeGregorianAndDOWFields(fFields[UCAL_JULIAN_DAY], ec);

    // Call framework method to have subclass compute its fields.
    // These must include, at a minimum, MONTH, DAY_OF_MONTH,
    // EXTENDED_YEAR, YEAR, DAY_OF_YEAR.  This method will call internalSet(),
    // which will update stamp[].
    handleComputeFields(fFields[UCAL_JULIAN_DAY], ec);

    // Compute week-related fields, based on the subclass-computed
    // fields computed by handleComputeFields().
    computeWeekFields(ec);

    // Compute time-related fields.  These are indepent of the date and
    // of the subclass algorithm.  They depend only on the local zone
    // wall milliseconds in day.
    int32_t millisInDay =  (int32_t) (localMillis - (days * kOneDay));
    fFields[UCAL_MILLISECONDS_IN_DAY] = millisInDay;
    fFields[UCAL_MILLISECOND] = millisInDay % 1000;
    millisInDay /= 1000;
    fFields[UCAL_SECOND] = millisInDay % 60;
    millisInDay /= 60;
    fFields[UCAL_MINUTE] = millisInDay % 60;
    millisInDay /= 60;
    fFields[UCAL_HOUR_OF_DAY] = millisInDay;
    fFields[UCAL_AM_PM] = millisInDay / 12; // Assume AM == 0
    fFields[UCAL_HOUR] = millisInDay % 12;
    fFields[UCAL_ZONE_OFFSET] = rawOffset;
    fFields[UCAL_DST_OFFSET] = dstOffset;
}

uint8_t Calendar::julianDayToDayOfWeek(double julian)
{
    // If julian is negative, then julian%7 will be negative, so we adjust
    // accordingly.  We add 1 because Julian day 0 is Monday.
    int8_t dayOfWeek = (int8_t) uprv_fmod(julian + 1, 7);

    uint8_t result = (uint8_t)(dayOfWeek + ((dayOfWeek < 0) ? (7+UCAL_SUNDAY ) : UCAL_SUNDAY));
    return result;
}

/**
* Compute the Gregorian calendar year, month, and day of month from
* the given Julian day.  These values are not stored in fields, but in
* member variables gregorianXxx.  Also compute the DAY_OF_WEEK and
* DOW_LOCAL fields.
*/
void Calendar::computeGregorianAndDOWFields(int32_t julianDay, UErrorCode &ec)
{
    computeGregorianFields(julianDay, ec);

    // Compute day of week: JD 0 = Monday
    int32_t dow = julianDayToDayOfWeek(julianDay);
    internalSet(UCAL_DAY_OF_WEEK,dow);

    // Calculate 1-based localized day of week
    int32_t dowLocal = dow - getFirstDayOfWeek() + 1;
    if (dowLocal < 1) {
        dowLocal += 7;
    }
    internalSet(UCAL_DOW_LOCAL,dowLocal);
    fFields[UCAL_DOW_LOCAL] = dowLocal;
}

/**
* Compute the Gregorian calendar year, month, and day of month from the
* Julian day.  These values are not stored in fields, but in member
* variables gregorianXxx.  They are used for time zone computations and by
* subclasses that are Gregorian derivatives.  Subclasses may call this
* method to perform a Gregorian calendar millis->fields computation.
*/
void Calendar::computeGregorianFields(int32_t julianDay, UErrorCode & /* ec */) {
    int32_t gregorianDayOfWeekUnused;
    Grego::dayToFields(julianDay - kEpochStartAsJulianDay, fGregorianYear, fGregorianMonth, fGregorianDayOfMonth, gregorianDayOfWeekUnused, fGregorianDayOfYear);
}

/**
* Compute the fields WEEK_OF_YEAR, YEAR_WOY, WEEK_OF_MONTH,
* DAY_OF_WEEK_IN_MONTH, and DOW_LOCAL from EXTENDED_YEAR, YEAR,
* DAY_OF_WEEK, and DAY_OF_YEAR.  The latter fields are computed by the
* subclass based on the calendar system.
*
* <p>The YEAR_WOY field is computed simplistically.  It is equal to YEAR
* most of the time, but at the year boundary it may be adjusted to YEAR-1
* or YEAR+1 to reflect the overlap of a week into an adjacent year.  In
* this case, a simple increment or decrement is performed on YEAR, even
* though this may yield an invalid YEAR value.  For instance, if the YEAR
* is part of a calendar system with an N-year cycle field CYCLE, then
* incrementing the YEAR may involve incrementing CYCLE and setting YEAR
* back to 0 or 1.  This is not handled by this code, and in fact cannot be
* simply handled without having subclasses define an entire parallel set of
* fields for fields larger than or equal to a year.  This additional
* complexity is not warranted, since the intention of the YEAR_WOY field is
* to support ISO 8601 notation, so it will typically be used with a
* proleptic Gregorian calendar, which has no field larger than a year.
*/
void Calendar::computeWeekFields(UErrorCode &ec) {
    if(U_FAILURE(ec)) { 
        return;
    }
    int32_t eyear = fFields[UCAL_EXTENDED_YEAR];
    int32_t dayOfWeek = fFields[UCAL_DAY_OF_WEEK];
    int32_t dayOfYear = fFields[UCAL_DAY_OF_YEAR];

    // WEEK_OF_YEAR start
    // Compute the week of the year.  For the Gregorian calendar, valid week
    // numbers run from 1 to 52 or 53, depending on the year, the first day
    // of the week, and the minimal days in the first week.  For other
    // calendars, the valid range may be different -- it depends on the year
    // length.  Days at the start of the year may fall into the last week of
    // the previous year; days at the end of the year may fall into the
    // first week of the next year.  ASSUME that the year length is less than
    // 7000 days.
    int32_t yearOfWeekOfYear = eyear;
    int32_t relDow = (dayOfWeek + 7 - getFirstDayOfWeek()) % 7; // 0..6
    int32_t relDowJan1 = (dayOfWeek - dayOfYear + 7001 - getFirstDayOfWeek()) % 7; // 0..6
    int32_t woy = (dayOfYear - 1 + relDowJan1) / 7; // 0..53
    if ((7 - relDowJan1) >= getMinimalDaysInFirstWeek()) {
        ++woy;
    }

    // Adjust for weeks at the year end that overlap into the previous or
    // next calendar year.
    if (woy == 0) {
        // We are the last week of the previous year.
        // Check to see if we are in the last week; if so, we need
        // to handle the case in which we are the first week of the
        // next year.

        int32_t prevDoy = dayOfYear + handleGetYearLength(eyear - 1);
        woy = weekNumber(prevDoy, dayOfWeek);
        yearOfWeekOfYear--;
    } else {
        int32_t lastDoy = handleGetYearLength(eyear);
        // Fast check: For it to be week 1 of the next year, the DOY
        // must be on or after L-5, where L is yearLength(), then it
        // cannot possibly be week 1 of the next year:
        //          L-5                  L
        // doy: 359 360 361 362 363 364 365 001
        // dow:      1   2   3   4   5   6   7
        if (dayOfYear >= (lastDoy - 5)) {
            int32_t lastRelDow = (relDow + lastDoy - dayOfYear) % 7;
            if (lastRelDow < 0) {
                lastRelDow += 7;
            }
            if (((6 - lastRelDow) >= getMinimalDaysInFirstWeek()) &&
                ((dayOfYear + 7 - relDow) > lastDoy)) {
                    woy = 1;
                    yearOfWeekOfYear++;
                }
        }
    }
    fFields[UCAL_WEEK_OF_YEAR] = woy;
    fFields[UCAL_YEAR_WOY] = yearOfWeekOfYear;
    // WEEK_OF_YEAR end

    int32_t dayOfMonth = fFields[UCAL_DAY_OF_MONTH];
    fFields[UCAL_WEEK_OF_MONTH] = weekNumber(dayOfMonth, dayOfWeek);
    fFields[UCAL_DAY_OF_WEEK_IN_MONTH] = (dayOfMonth-1) / 7 + 1;
#if defined (U_DEBUG_CAL)
    if(fFields[UCAL_DAY_OF_WEEK_IN_MONTH]==0) fprintf(stderr, "%s:%d: DOWIM %d on %g\n", 
        __FILE__, __LINE__,fFields[UCAL_DAY_OF_WEEK_IN_MONTH], fTime);
#endif
}


int32_t Calendar::weekNumber(int32_t desiredDay, int32_t dayOfPeriod, int32_t dayOfWeek)
{
    // Determine the day of the week of the first day of the period
    // in question (either a year or a month).  Zero represents the
    // first day of the week on this calendar.
    int32_t periodStartDayOfWeek = (dayOfWeek - getFirstDayOfWeek() - dayOfPeriod + 1) % 7;
    if (periodStartDayOfWeek < 0) periodStartDayOfWeek += 7;

    // Compute the week number.  Initially, ignore the first week, which
    // may be fractional (or may not be).  We add periodStartDayOfWeek in
    // order to fill out the first week, if it is fractional.
    int32_t weekNo = (desiredDay + periodStartDayOfWeek - 1)/7;

    // If the first week is long enough, then count it.  If
    // the minimal days in the first week is one, or if the period start
    // is zero, we always increment weekNo.
    if ((7 - periodStartDayOfWeek) >= getMinimalDaysInFirstWeek()) ++weekNo;

    return weekNo;
}

void Calendar::handleComputeFields(int32_t /* julianDay */, UErrorCode &/* status */)
{
    internalSet(UCAL_MONTH, getGregorianMonth());
    internalSet(UCAL_DAY_OF_MONTH, getGregorianDayOfMonth());
    internalSet(UCAL_DAY_OF_YEAR, getGregorianDayOfYear());
    int32_t eyear = getGregorianYear();
    internalSet(UCAL_EXTENDED_YEAR, eyear);
    int32_t era = GregorianCalendar::AD;
    if (eyear < 1) {
        era = GregorianCalendar::BC;
        eyear = 1 - eyear;
    }
    internalSet(UCAL_ERA, era);
    internalSet(UCAL_YEAR, eyear);
}
// -------------------------------------


void Calendar::roll(EDateFields field, int32_t amount, UErrorCode& status) 
{
    roll((UCalendarDateFields)field, amount, status);
}

void Calendar::roll(UCalendarDateFields field, int32_t amount, UErrorCode& status)
{
    if (amount == 0) {
        return; // Nothing to do
    }

    complete(status);

    if(U_FAILURE(status)) {
        return;
    }
    switch (field) {
    case UCAL_DAY_OF_MONTH:
    case UCAL_AM_PM:
    case UCAL_MINUTE:
    case UCAL_SECOND:
    case UCAL_MILLISECOND:
    case UCAL_MILLISECONDS_IN_DAY:
    case UCAL_ERA:
        // These are the standard roll instructions.  These work for all
        // simple cases, that is, cases in which the limits are fixed, such
        // as the hour, the day of the month, and the era.
        {
            int32_t min = getActualMinimum(field,status);
            int32_t max = getActualMaximum(field,status);
            int32_t gap = max - min + 1;

            int32_t value = internalGet(field) + amount;
            value = (value - min) % gap;
            if (value < 0) {
                value += gap;
            }
            value += min;

            set(field, value);
            return;
        }

    case UCAL_HOUR:
    case UCAL_HOUR_OF_DAY:
        // Rolling the hour is difficult on the ONSET and CEASE days of
        // daylight savings.  For example, if the change occurs at
        // 2 AM, we have the following progression:
        // ONSET: 12 Std -> 1 Std -> 3 Dst -> 4 Dst
        // CEASE: 12 Dst -> 1 Dst -> 1 Std -> 2 Std
        // To get around this problem we don't use fields; we manipulate
        // the time in millis directly.
        {
            // Assume min == 0 in calculations below
            double start = getTimeInMillis(status);
            int32_t oldHour = internalGet(field);
            int32_t max = getMaximum(field);
            int32_t newHour = (oldHour + amount) % (max + 1);
            if (newHour < 0) {
                newHour += max + 1;
            }
            setTimeInMillis(start + kOneHour * (newHour - oldHour),status);
            return;
        }

    case UCAL_MONTH:
        // Rolling the month involves both pinning the final value
        // and adjusting the DAY_OF_MONTH if necessary.  We only adjust the
        // DAY_OF_MONTH if, after updating the MONTH field, it is illegal.
        // E.g., <jan31>.roll(MONTH, 1) -> <feb28> or <feb29>.
        {
            int32_t max = getActualMaximum(UCAL_MONTH, status);
            int32_t mon = (internalGet(UCAL_MONTH) + amount) % (max+1);

            if (mon < 0) {
                mon += (max + 1);
            }
            set(UCAL_MONTH, mon);

            // Keep the day of month in range.  We don't want to spill over
            // into the next month; e.g., we don't want jan31 + 1 mo -> feb31 ->
            // mar3.
            pinField(UCAL_DAY_OF_MONTH,status);
            return;
        }

    case UCAL_YEAR:
    case UCAL_YEAR_WOY:
        {
            // * If era==0 and years go backwards in time, change sign of amount.
            // * Until we have new API per #9393, we temporarily hardcode knowledge of
            //   which calendars have era 0 years that go backwards.
            UBool era0WithYearsThatGoBackwards = FALSE;
            int32_t era = get(UCAL_ERA, status);
            if (era == 0) {
                const char * calType = getType();
                if ( uprv_strcmp(calType,"gregorian")==0 || uprv_strcmp(calType,"roc")==0 || uprv_strcmp(calType,"coptic")==0 ) {
                    amount = -amount;
                    era0WithYearsThatGoBackwards = TRUE;
                }
            }
            int32_t newYear = internalGet(field) + amount;
            if (era > 0 || newYear >= 1) {
                int32_t maxYear = getActualMaximum(field, status);
                if (maxYear < 32768) {
                    // this era has real bounds, roll should wrap years
                    if (newYear < 1) {
                        newYear = maxYear - ((-newYear) % maxYear);
                    } else if (newYear > maxYear) {
                        newYear = ((newYear - 1) % maxYear) + 1;
                    }
                // else era is unbounded, just pin low year instead of wrapping
                } else if (newYear < 1) {
                    newYear = 1;
                }
            // else we are in era 0 with newYear < 1;
            // calendars with years that go backwards must pin the year value at 0,
            // other calendars can have years < 0 in era 0
            } else if (era0WithYearsThatGoBackwards) {
                newYear = 1;
            }
            set(field, newYear);
            pinField(UCAL_MONTH,status);
            pinField(UCAL_DAY_OF_MONTH,status);
            return;
        }

    case UCAL_EXTENDED_YEAR:
        // Rolling the year can involve pinning the DAY_OF_MONTH.
        set(field, internalGet(field) + amount);
        pinField(UCAL_MONTH,status);
        pinField(UCAL_DAY_OF_MONTH,status);
        return;

    case UCAL_WEEK_OF_MONTH:
        {
            // This is tricky, because during the roll we may have to shift
            // to a different day of the week.  For example:

            //    s  m  t  w  r  f  s
            //          1  2  3  4  5
            //    6  7  8  9 10 11 12

            // When rolling from the 6th or 7th back one week, we go to the
            // 1st (assuming that the first partial week counts).  The same
            // thing happens at the end of the month.

            // The other tricky thing is that we have to figure out whether
            // the first partial week actually counts or not, based on the
            // minimal first days in the week.  And we have to use the
            // correct first day of the week to delineate the week
            // boundaries.

            // Here's our algorithm.  First, we find the real boundaries of
            // the month.  Then we discard the first partial week if it
            // doesn't count in this locale.  Then we fill in the ends with
            // phantom days, so that the first partial week and the last
            // partial week are full weeks.  We then have a nice square
            // block of weeks.  We do the usual rolling within this block,
            // as is done elsewhere in this method.  If we wind up on one of
            // the phantom days that we added, we recognize this and pin to
            // the first or the last day of the month.  Easy, eh?

            // Normalize the DAY_OF_WEEK so that 0 is the first day of the week
            // in this locale.  We have dow in 0..6.
            int32_t dow = internalGet(UCAL_DAY_OF_WEEK) - getFirstDayOfWeek();
            if (dow < 0) dow += 7;

            // Find the day of the week (normalized for locale) for the first
            // of the month.
            int32_t fdm = (dow - internalGet(UCAL_DAY_OF_MONTH) + 1) % 7;
            if (fdm < 0) fdm += 7;

            // Get the first day of the first full week of the month,
            // including phantom days, if any.  Figure out if the first week
            // counts or not; if it counts, then fill in phantom days.  If
            // not, advance to the first real full week (skip the partial week).
            int32_t start;
            if ((7 - fdm) < getMinimalDaysInFirstWeek())
                start = 8 - fdm; // Skip the first partial week
            else
                start = 1 - fdm; // This may be zero or negative

            // Get the day of the week (normalized for locale) for the last
            // day of the month.
            int32_t monthLen = getActualMaximum(UCAL_DAY_OF_MONTH, status);
            int32_t ldm = (monthLen - internalGet(UCAL_DAY_OF_MONTH) + dow) % 7;
            // We know monthLen >= DAY_OF_MONTH so we skip the += 7 step here.

            // Get the limit day for the blocked-off rectangular month; that
            // is, the day which is one past the last day of the month,
            // after the month has already been filled in with phantom days
            // to fill out the last week.  This day has a normalized DOW of 0.
            int32_t limit = monthLen + 7 - ldm;

            // Now roll between start and (limit - 1).
            int32_t gap = limit - start;
            int32_t day_of_month = (internalGet(UCAL_DAY_OF_MONTH) + amount*7 -
                start) % gap;
            if (day_of_month < 0) day_of_month += gap;
            day_of_month += start;

            // Finally, pin to the real start and end of the month.
            if (day_of_month < 1) day_of_month = 1;
            if (day_of_month > monthLen) day_of_month = monthLen;

            // Set the DAY_OF_MONTH.  We rely on the fact that this field
            // takes precedence over everything else (since all other fields
            // are also set at this point).  If this fact changes (if the
            // disambiguation algorithm changes) then we will have to unset
            // the appropriate fields here so that DAY_OF_MONTH is attended
            // to.
            set(UCAL_DAY_OF_MONTH, day_of_month);
            return;
        }
    case UCAL_WEEK_OF_YEAR:
        {
            // This follows the outline of WEEK_OF_MONTH, except it applies
            // to the whole year.  Please see the comment for WEEK_OF_MONTH
            // for general notes.

            // Normalize the DAY_OF_WEEK so that 0 is the first day of the week
            // in this locale.  We have dow in 0..6.
            int32_t dow = internalGet(UCAL_DAY_OF_WEEK) - getFirstDayOfWeek();
            if (dow < 0) dow += 7;

            // Find the day of the week (normalized for locale) for the first
            // of the year.
            int32_t fdy = (dow - internalGet(UCAL_DAY_OF_YEAR) + 1) % 7;
            if (fdy < 0) fdy += 7;

            // Get the first day of the first full week of the year,
            // including phantom days, if any.  Figure out if the first week
            // counts or not; if it counts, then fill in phantom days.  If
            // not, advance to the first real full week (skip the partial week).
            int32_t start;
            if ((7 - fdy) < getMinimalDaysInFirstWeek())
                start = 8 - fdy; // Skip the first partial week
            else
                start = 1 - fdy; // This may be zero or negative

            // Get the day of the week (normalized for locale) for the last
            // day of the year.
            int32_t yearLen = getActualMaximum(UCAL_DAY_OF_YEAR,status);
            int32_t ldy = (yearLen - internalGet(UCAL_DAY_OF_YEAR) + dow) % 7;
            // We know yearLen >= DAY_OF_YEAR so we skip the += 7 step here.

            // Get the limit day for the blocked-off rectangular year; that
            // is, the day which is one past the last day of the year,
            // after the year has already been filled in with phantom days
            // to fill out the last week.  This day has a normalized DOW of 0.
            int32_t limit = yearLen + 7 - ldy;

            // Now roll between start and (limit - 1).
            int32_t gap = limit - start;
            int32_t day_of_year = (internalGet(UCAL_DAY_OF_YEAR) + amount*7 -
                start) % gap;
            if (day_of_year < 0) day_of_year += gap;
            day_of_year += start;

            // Finally, pin to the real start and end of the month.
            if (day_of_year < 1) day_of_year = 1;
            if (day_of_year > yearLen) day_of_year = yearLen;

            // Make sure that the year and day of year are attended to by
            // clearing other fields which would normally take precedence.
            // If the disambiguation algorithm is changed, this section will
            // have to be updated as well.
            set(UCAL_DAY_OF_YEAR, day_of_year);
            clear(UCAL_MONTH);
            return;
        }
    case UCAL_DAY_OF_YEAR:
        {
            // Roll the day of year using millis.  Compute the millis for
            // the start of the year, and get the length of the year.
            double delta = amount * kOneDay; // Scale up from days to millis
            double min2 = internalGet(UCAL_DAY_OF_YEAR)-1;
            min2 *= kOneDay;
            min2 = internalGetTime() - min2;

            //      double min2 = internalGetTime() - (internalGet(UCAL_DAY_OF_YEAR) - 1.0) * kOneDay;
            double newtime;

            double yearLength = getActualMaximum(UCAL_DAY_OF_YEAR,status);
            double oneYear = yearLength;
            oneYear *= kOneDay;
            newtime = uprv_fmod((internalGetTime() + delta - min2), oneYear);
            if (newtime < 0) newtime += oneYear;
            setTimeInMillis(newtime + min2, status);
            return;
        }
    case UCAL_DAY_OF_WEEK:
    case UCAL_DOW_LOCAL:
        {
            // Roll the day of week using millis.  Compute the millis for
            // the start of the week, using the first day of week setting.
            // Restrict the millis to [start, start+7days).
            double delta = amount * kOneDay; // Scale up from days to millis
            // Compute the number of days before the current day in this
            // week.  This will be a value 0..6.
            int32_t leadDays = internalGet(field);
            leadDays -= (field == UCAL_DAY_OF_WEEK) ? getFirstDayOfWeek() : 1;
            if (leadDays < 0) leadDays += 7;
            double min2 = internalGetTime() - leadDays * kOneDay;
            double newtime = uprv_fmod((internalGetTime() + delta - min2), kOneWeek);
            if (newtime < 0) newtime += kOneWeek;
            setTimeInMillis(newtime + min2, status);
            return;
        }
    case UCAL_DAY_OF_WEEK_IN_MONTH:
        {
            // Roll the day of week in the month using millis.  Determine
            // the first day of the week in the month, and then the last,
            // and then roll within that range.
            double delta = amount * kOneWeek; // Scale up from weeks to millis
            // Find the number of same days of the week before this one
            // in this month.
            int32_t preWeeks = (internalGet(UCAL_DAY_OF_MONTH) - 1) / 7;
            // Find the number of same days of the week after this one
            // in this month.
            int32_t postWeeks = (getActualMaximum(UCAL_DAY_OF_MONTH,status) -
                internalGet(UCAL_DAY_OF_MONTH)) / 7;
            // From these compute the min and gap millis for rolling.
            double min2 = internalGetTime() - preWeeks * kOneWeek;
            double gap2 = kOneWeek * (preWeeks + postWeeks + 1); // Must add 1!
            // Roll within this range
            double newtime = uprv_fmod((internalGetTime() + delta - min2), gap2);
            if (newtime < 0) newtime += gap2;
            setTimeInMillis(newtime + min2, status);
            return;
        }
    case UCAL_JULIAN_DAY:
        set(field, internalGet(field) + amount);
        return;
    default:
        // Other fields cannot be rolled by this method
#if defined (U_DEBUG_CAL)
        fprintf(stderr, "%s:%d: ILLEGAL ARG because of roll on non-rollable field %s\n", 
            __FILE__, __LINE__,fldName(field));
#endif
        status = U_ILLEGAL_ARGUMENT_ERROR;
    }
}

void Calendar::add(EDateFields field, int32_t amount, UErrorCode& status)
{
    Calendar::add((UCalendarDateFields)field, amount, status);
}

// -------------------------------------
void Calendar::add(UCalendarDateFields field, int32_t amount, UErrorCode& status)
{
    if (amount == 0) {
        return;   // Do nothing!
    }

    // We handle most fields in the same way.  The algorithm is to add
    // a computed amount of millis to the current millis.  The only
    // wrinkle is with DST (and/or a change to the zone's UTC offset, which
    // we'll include with DST) -- for some fields, like the DAY_OF_MONTH,
    // we don't want the wall time to shift due to changes in DST.  If the
    // result of the add operation is to move from DST to Standard, or
    // vice versa, we need to adjust by an hour forward or back,
    // respectively.  For such fields we set keepWallTimeInvariant to TRUE.

    // We only adjust the DST for fields larger than an hour.  For
    // fields smaller than an hour, we cannot adjust for DST without
    // causing problems.  for instance, if you add one hour to April 5,
    // 1998, 1:00 AM, in PST, the time becomes "2:00 AM PDT" (an
    // illegal value), but then the adjustment sees the change and
    // compensates by subtracting an hour.  As a result the time
    // doesn't advance at all.

    // For some fields larger than a day, such as a UCAL_MONTH, we pin the
    // UCAL_DAY_OF_MONTH.  This allows <March 31>.add(UCAL_MONTH, 1) to be
    // <April 30>, rather than <April 31> => <May 1>.

    double delta = amount; // delta in ms
    UBool keepWallTimeInvariant = TRUE;

    switch (field) {
    case UCAL_ERA:
        set(field, get(field, status) + amount);
        pinField(UCAL_ERA, status);
        return;

    case UCAL_YEAR:
    case UCAL_YEAR_WOY:
      {
        // * If era=0 and years go backwards in time, change sign of amount.
        // * Until we have new API per #9393, we temporarily hardcode knowledge of
        //   which calendars have era 0 years that go backwards.
        // * Note that for UCAL_YEAR (but not UCAL_YEAR_WOY) we could instead handle
        //   this by applying the amount to the UCAL_EXTENDED_YEAR field; but since
        //   we would still need to handle UCAL_YEAR_WOY as below, might as well
        //   also handle UCAL_YEAR the same way.
        int32_t era = get(UCAL_ERA, status);
        if (era == 0) {
          const char * calType = getType();
          if ( uprv_strcmp(calType,"gregorian")==0 || uprv_strcmp(calType,"roc")==0 || uprv_strcmp(calType,"coptic")==0 ) {
            amount = -amount;
          }
        }
      }
      // Fall through into normal handling
    case UCAL_EXTENDED_YEAR:
    case UCAL_MONTH:
      {
        UBool oldLenient = isLenient();
        setLenient(TRUE);
        set(field, get(field, status) + amount);
        pinField(UCAL_DAY_OF_MONTH, status);
        if(oldLenient==FALSE) {
          complete(status); /* force recalculate */
          setLenient(oldLenient);
        }
      }
      return;

    case UCAL_WEEK_OF_YEAR:
    case UCAL_WEEK_OF_MONTH:
    case UCAL_DAY_OF_WEEK_IN_MONTH:
        delta *= kOneWeek;
        break;

    case UCAL_AM_PM:
        delta *= 12 * kOneHour;
        break;

    case UCAL_DAY_OF_MONTH:
    case UCAL_DAY_OF_YEAR:
    case UCAL_DAY_OF_WEEK:
    case UCAL_DOW_LOCAL:
    case UCAL_JULIAN_DAY:
        delta *= kOneDay;
        break;

    case UCAL_HOUR_OF_DAY:
    case UCAL_HOUR:
        delta *= kOneHour;
        keepWallTimeInvariant = FALSE;
        break;

    case UCAL_MINUTE:
        delta *= kOneMinute;
        keepWallTimeInvariant = FALSE;
        break;

    case UCAL_SECOND:
        delta *= kOneSecond;
        keepWallTimeInvariant = FALSE;
        break;

    case UCAL_MILLISECOND:
    case UCAL_MILLISECONDS_IN_DAY:
        keepWallTimeInvariant = FALSE;
        break;

    default:
#if defined (U_DEBUG_CAL)
        fprintf(stderr, "%s:%d: ILLEGAL ARG because field %s not addable",
            __FILE__, __LINE__, fldName(field));
#endif
        status = U_ILLEGAL_ARGUMENT_ERROR;
        return;
        //  throw new IllegalArgumentException("Calendar.add(" + fieldName(field) +
        //                                     ") not supported");
    }

    // In order to keep the wall time invariant (for fields where this is
    // appropriate), check the combined DST & ZONE offset before and
    // after the add() operation. If it changes, then adjust the millis
    // to compensate.
    int32_t prevOffset = 0;
    int32_t prevWallTime = 0;
    if (keepWallTimeInvariant) {
        prevOffset = get(UCAL_DST_OFFSET, status) + get(UCAL_ZONE_OFFSET, status);
        prevWallTime = get(UCAL_MILLISECONDS_IN_DAY, status);
    }

    setTimeInMillis(getTimeInMillis(status) + delta, status);

    if (keepWallTimeInvariant) {
        int32_t newWallTime = get(UCAL_MILLISECONDS_IN_DAY, status);
        if (newWallTime != prevWallTime) {
            // There is at least one zone transition between the base
            // time and the result time. As the result, wall time has
            // changed.
            UDate t = internalGetTime();
            int32_t newOffset = get(UCAL_DST_OFFSET, status) + get(UCAL_ZONE_OFFSET, status);
            if (newOffset != prevOffset) {
                // When the difference of the previous UTC offset and
                // the new UTC offset exceeds 1 full day, we do not want
                // to roll over/back the date. For now, this only happens
                // in Samoa (Pacific/Apia) on Dec 30, 2011. See ticket:9452.
                int32_t adjAmount = prevOffset - newOffset;
                adjAmount = adjAmount >= 0 ? adjAmount % (int32_t)kOneDay : -(-adjAmount % (int32_t)kOneDay);
                if (adjAmount != 0) {
                    setTimeInMillis(t + adjAmount, status);
                    newWallTime = get(UCAL_MILLISECONDS_IN_DAY, status);
                }
                if (newWallTime != prevWallTime) {
                    // The result wall time or adjusted wall time was shifted because
                    // the target wall time does not exist on the result date.
                    switch (fSkippedWallTime) {
                    case UCAL_WALLTIME_FIRST:
                        if (adjAmount > 0) {
                            setTimeInMillis(t, status);
                        }
                        break;
                    case UCAL_WALLTIME_LAST:
                        if (adjAmount < 0) {
                            setTimeInMillis(t, status);
                        }
                        break;
                    case UCAL_WALLTIME_NEXT_VALID:
                        UDate tmpT = adjAmount > 0 ? internalGetTime() : t;
                        UDate immediatePrevTrans;
                        UBool hasTransition = getImmediatePreviousZoneTransition(tmpT, &immediatePrevTrans, status);
                        if (U_SUCCESS(status) && hasTransition) {
                            setTimeInMillis(immediatePrevTrans, status);
                        }
                        break;
                    }
                }
            }
        }
    } 
}

// -------------------------------------
int32_t Calendar::fieldDifference(UDate when, EDateFields field, UErrorCode& status) {
    return fieldDifference(when, (UCalendarDateFields) field, status);
}

int32_t Calendar::fieldDifference(UDate targetMs, UCalendarDateFields field, UErrorCode& ec) {
    if (U_FAILURE(ec)) return 0;
    int32_t min = 0;
    double startMs = getTimeInMillis(ec);
    // Always add from the start millis.  This accomodates
    // operations like adding years from February 29, 2000 up to
    // February 29, 2004.  If 1, 1, 1, 1 is added to the year
    // field, the DOM gets pinned to 28 and stays there, giving an
    // incorrect DOM difference of 1.  We have to add 1, reset, 2,
    // reset, 3, reset, 4.
    if (startMs < targetMs) {
        int32_t max = 1;
        // Find a value that is too large
        while (U_SUCCESS(ec)) {
            setTimeInMillis(startMs, ec);
            add(field, max, ec);
            double ms = getTimeInMillis(ec);
            if (ms == targetMs) {
                return max;
            } else if (ms > targetMs) {
                break;
            } else if (max < INT32_MAX) {
                min = max;
                max <<= 1;
                if (max < 0) {
                    max = INT32_MAX;
                }
            } else {
                // Field difference too large to fit into int32_t
#if defined (U_DEBUG_CAL)
                fprintf(stderr, "%s:%d: ILLEGAL ARG because field %s's max too large for int32_t\n",
                    __FILE__, __LINE__, fldName(field));
#endif
                ec = U_ILLEGAL_ARGUMENT_ERROR;
            }
        }
        // Do a binary search
        while ((max - min) > 1 && U_SUCCESS(ec)) {
            int32_t t = min + (max - min)/2; // make sure intermediate values don't exceed INT32_MAX
            setTimeInMillis(startMs, ec);
            add(field, t, ec);
            double ms = getTimeInMillis(ec);
            if (ms == targetMs) {
                return t;
            } else if (ms > targetMs) {
                max = t;
            } else {
                min = t;
            }
        }
    } else if (startMs > targetMs) {
        int32_t max = -1;
        // Find a value that is too small
        while (U_SUCCESS(ec)) {
            setTimeInMillis(startMs, ec);
            add(field, max, ec);
            double ms = getTimeInMillis(ec);
            if (ms == targetMs) {
                return max;
            } else if (ms < targetMs) {
                break;
            } else {
                min = max;
                max <<= 1;
                if (max == 0) {
                    // Field difference too large to fit into int32_t
#if defined (U_DEBUG_CAL)
                    fprintf(stderr, "%s:%d: ILLEGAL ARG because field %s's max too large for int32_t\n",
                        __FILE__, __LINE__, fldName(field));
#endif
                    ec = U_ILLEGAL_ARGUMENT_ERROR;
                }
            }
        }
        // Do a binary search
        while ((min - max) > 1 && U_SUCCESS(ec)) {
            int32_t t = min + (max - min)/2; // make sure intermediate values don't exceed INT32_MAX
            setTimeInMillis(startMs, ec);
            add(field, t, ec);
            double ms = getTimeInMillis(ec);
            if (ms == targetMs) {
                return t;
            } else if (ms < targetMs) {
                max = t;
            } else {
                min = t;
            }
        }
    }
    // Set calendar to end point
    setTimeInMillis(startMs, ec);
    add(field, min, ec);

    /* Test for buffer overflows */
    if(U_FAILURE(ec)) {
        return 0;
    }
    return min;
}

// -------------------------------------

void
Calendar::adoptTimeZone(TimeZone* zone)
{
    // Do nothing if passed-in zone is NULL
    if (zone == NULL) return;

    // fZone should always be non-null
    if (fZone != NULL) delete fZone;
    fZone = zone;

    // if the zone changes, we need to recompute the time fields
    fAreFieldsSet = FALSE;
}

// -------------------------------------
void
Calendar::setTimeZone(const TimeZone& zone)
{
    adoptTimeZone(zone.clone());
}

// -------------------------------------

const TimeZone&
Calendar::getTimeZone() const
{
    return *fZone;
}

// -------------------------------------

TimeZone*
Calendar::orphanTimeZone()
{
    TimeZone *z = fZone;
    // we let go of the time zone; the new time zone is the system default time zone
    fZone = TimeZone::createDefault();
    return z;
}

// -------------------------------------

void
Calendar::setLenient(UBool lenient)
{
    fLenient = lenient;
}

// -------------------------------------

UBool
Calendar::isLenient() const
{
    return fLenient;
}

// -------------------------------------

void
Calendar::setRepeatedWallTimeOption(UCalendarWallTimeOption option)
{
    if (option == UCAL_WALLTIME_LAST || option == UCAL_WALLTIME_FIRST) {
        fRepeatedWallTime = option;
    }
}

// -------------------------------------

UCalendarWallTimeOption
Calendar::getRepeatedWallTimeOption(void) const
{
    return fRepeatedWallTime;
}

// -------------------------------------

void
Calendar::setSkippedWallTimeOption(UCalendarWallTimeOption option)
{
    fSkippedWallTime = option;
}

// -------------------------------------

UCalendarWallTimeOption
Calendar::getSkippedWallTimeOption(void) const
{
    return fSkippedWallTime;
}

// -------------------------------------

void
Calendar::setFirstDayOfWeek(UCalendarDaysOfWeek value)
{
    if (fFirstDayOfWeek != value &&
        value >= UCAL_SUNDAY && value <= UCAL_SATURDAY) {
            fFirstDayOfWeek = value;
            fAreFieldsSet = FALSE;
        }
}

// -------------------------------------

Calendar::EDaysOfWeek
Calendar::getFirstDayOfWeek() const
{
    return (Calendar::EDaysOfWeek)fFirstDayOfWeek;
}

UCalendarDaysOfWeek
Calendar::getFirstDayOfWeek(UErrorCode & /*status*/) const
{
    return fFirstDayOfWeek;
}
// -------------------------------------

void
Calendar::setMinimalDaysInFirstWeek(uint8_t value)
{
    // Values less than 1 have the same effect as 1; values greater
    // than 7 have the same effect as 7. However, we normalize values
    // so operator== and so forth work.
    if (value < 1) {
        value = 1;
    } else if (value > 7) {
        value = 7;
    }
    if (fMinimalDaysInFirstWeek != value) {
        fMinimalDaysInFirstWeek = value;
        fAreFieldsSet = FALSE;
    }
}

// -------------------------------------

uint8_t
Calendar::getMinimalDaysInFirstWeek() const
{
    return fMinimalDaysInFirstWeek;
}

// -------------------------------------
// weekend functions, just dummy implementations for now (for API freeze)

UCalendarWeekdayType
Calendar::getDayOfWeekType(UCalendarDaysOfWeek dayOfWeek, UErrorCode &status) const
{
    if (U_FAILURE(status)) {
        return UCAL_WEEKDAY;
    }
    if (dayOfWeek < UCAL_SUNDAY || dayOfWeek > UCAL_SATURDAY) {
        status = U_ILLEGAL_ARGUMENT_ERROR;
        return UCAL_WEEKDAY;
    }
    if (fWeekendOnset == fWeekendCease) {
        if (dayOfWeek != fWeekendOnset)
            return UCAL_WEEKDAY;
        return (fWeekendOnsetMillis == 0) ? UCAL_WEEKEND : UCAL_WEEKEND_ONSET;
    }
    if (fWeekendOnset < fWeekendCease) {
        if (dayOfWeek < fWeekendOnset || dayOfWeek > fWeekendCease) {
            return UCAL_WEEKDAY;
        }
    } else {
        if (dayOfWeek > fWeekendCease && dayOfWeek < fWeekendOnset) {
            return UCAL_WEEKDAY;
        }
    }
    if (dayOfWeek == fWeekendOnset) {
        return (fWeekendOnsetMillis == 0) ? UCAL_WEEKEND : UCAL_WEEKEND_ONSET;
    }
    if (dayOfWeek == fWeekendCease) {
        return (fWeekendCeaseMillis >= 86400000) ? UCAL_WEEKEND : UCAL_WEEKEND_CEASE;
    }
    return UCAL_WEEKEND;
}

int32_t
Calendar::getWeekendTransition(UCalendarDaysOfWeek dayOfWeek, UErrorCode &status) const
{
    if (U_FAILURE(status)) {
        return 0;
    }
    if (dayOfWeek == fWeekendOnset) {
        return fWeekendOnsetMillis;
    } else if (dayOfWeek == fWeekendCease) {
        return fWeekendCeaseMillis;
    }
    status = U_ILLEGAL_ARGUMENT_ERROR;
    return 0;
}

UBool
Calendar::isWeekend(UDate date, UErrorCode &status) const
{
    if (U_FAILURE(status)) {
        return FALSE;
    }
    // clone the calendar so we don't mess with the real one.
    Calendar *work = (Calendar*)this->clone();
    if (work == NULL) {
        status = U_MEMORY_ALLOCATION_ERROR;
        return FALSE;
    }
    UBool result = FALSE;
    work->setTime(date, status);
    if (U_SUCCESS(status)) {
        result = work->isWeekend();
    }
    delete work;
    return result;
}

UBool
Calendar::isWeekend(void) const
{
    UErrorCode status = U_ZERO_ERROR;
    UCalendarDaysOfWeek dayOfWeek = (UCalendarDaysOfWeek)get(UCAL_DAY_OF_WEEK, status);
    UCalendarWeekdayType dayType = getDayOfWeekType(dayOfWeek, status);
    if (U_SUCCESS(status)) {
        switch (dayType) {
            case UCAL_WEEKDAY:
                return FALSE;
            case UCAL_WEEKEND:
                return TRUE;
            case UCAL_WEEKEND_ONSET:
            case UCAL_WEEKEND_CEASE:
                // Use internalGet() because the above call to get() populated all fields.
                {
                    int32_t millisInDay = internalGet(UCAL_MILLISECONDS_IN_DAY);
                    int32_t transitionMillis = getWeekendTransition(dayOfWeek, status);
                    if (U_SUCCESS(status)) {
                        return (dayType == UCAL_WEEKEND_ONSET)?
                            (millisInDay >= transitionMillis):
                            (millisInDay <  transitionMillis);
                    }
                    // else fall through, return FALSE
                }
            default:
                break;
        }
    }
    return FALSE;
}

// ------------------------------------- limits

int32_t 
Calendar::getMinimum(EDateFields field) const {
    return getLimit((UCalendarDateFields) field,UCAL_LIMIT_MINIMUM);
}

int32_t
Calendar::getMinimum(UCalendarDateFields field) const
{
    return getLimit(field,UCAL_LIMIT_MINIMUM);
}

// -------------------------------------
int32_t
Calendar::getMaximum(EDateFields field) const
{
    return getLimit((UCalendarDateFields) field,UCAL_LIMIT_MAXIMUM);
}

int32_t
Calendar::getMaximum(UCalendarDateFields field) const
{
    return getLimit(field,UCAL_LIMIT_MAXIMUM);
}

// -------------------------------------
int32_t
Calendar::getGreatestMinimum(EDateFields field) const
{
    return getLimit((UCalendarDateFields)field,UCAL_LIMIT_GREATEST_MINIMUM);
}

int32_t
Calendar::getGreatestMinimum(UCalendarDateFields field) const
{
    return getLimit(field,UCAL_LIMIT_GREATEST_MINIMUM);
}

// -------------------------------------
int32_t
Calendar::getLeastMaximum(EDateFields field) const
{
    return getLimit((UCalendarDateFields) field,UCAL_LIMIT_LEAST_MAXIMUM);
}

int32_t
Calendar::getLeastMaximum(UCalendarDateFields field) const
{
    return getLimit( field,UCAL_LIMIT_LEAST_MAXIMUM);
}

// -------------------------------------
int32_t 
Calendar::getActualMinimum(EDateFields field, UErrorCode& status) const
{
    return getActualMinimum((UCalendarDateFields) field, status);
}

int32_t Calendar::getLimit(UCalendarDateFields field, ELimitType limitType) const {
    switch (field) {
    case UCAL_DAY_OF_WEEK:
    case UCAL_AM_PM:
    case UCAL_HOUR:
    case UCAL_HOUR_OF_DAY:
    case UCAL_MINUTE:
    case UCAL_SECOND:
    case UCAL_MILLISECOND:
    case UCAL_ZONE_OFFSET:
    case UCAL_DST_OFFSET:
    case UCAL_DOW_LOCAL:
    case UCAL_JULIAN_DAY:
    case UCAL_MILLISECONDS_IN_DAY:
    case UCAL_IS_LEAP_MONTH:
        return kCalendarLimits[field][limitType];

    case UCAL_WEEK_OF_MONTH:
        {
            int32_t limit;
            if (limitType == UCAL_LIMIT_MINIMUM) {
                limit = getMinimalDaysInFirstWeek() == 1 ? 1 : 0;
            } else if (limitType == UCAL_LIMIT_GREATEST_MINIMUM) {
                limit = 1;
            } else {
                int32_t minDaysInFirst = getMinimalDaysInFirstWeek();
                int32_t daysInMonth = handleGetLimit(UCAL_DAY_OF_MONTH, limitType);
                if (limitType == UCAL_LIMIT_LEAST_MAXIMUM) {
                    limit = (daysInMonth + (7 - minDaysInFirst)) / 7;
                } else { // limitType == UCAL_LIMIT_MAXIMUM
                    limit = (daysInMonth + 6 + (7 - minDaysInFirst)) / 7;
                }
            }
            return limit;
        }
    default:
        return handleGetLimit(field, limitType);
    }
}


int32_t
Calendar::getActualMinimum(UCalendarDateFields field, UErrorCode& status) const
{
    int32_t fieldValue = getGreatestMinimum(field);
    int32_t endValue = getMinimum(field);

    // if we know that the minimum value is always the same, just return it
    if (fieldValue == endValue) {
        return fieldValue;
    }

    // clone the calendar so we don't mess with the real one, and set it to
    // accept anything for the field values
    Calendar *work = (Calendar*)this->clone();
    if (work == NULL) {
        status = U_MEMORY_ALLOCATION_ERROR;
        return 0;
    }
    work->setLenient(TRUE);

    // now try each value from getLeastMaximum() to getMaximum() one by one until
    // we get a value that normalizes to another value.  The last value that
    // normalizes to itself is the actual minimum for the current date
    int32_t result = fieldValue;

    do {
        work->set(field, fieldValue);
        if (work->get(field, status) != fieldValue) {
            break;
        } 
        else {
            result = fieldValue;
            fieldValue--;
        }
    } while (fieldValue >= endValue);

    delete work;

    /* Test for buffer overflows */
    if(U_FAILURE(status)) {
        return 0;
    }
    return result;
}

// -------------------------------------



/**
* Ensure that each field is within its valid range by calling {@link
* #validateField(int)} on each field that has been set.  This method
* should only be called if this calendar is not lenient.
* @see #isLenient
* @see #validateField(int)
*/
void Calendar::validateFields(UErrorCode &status) {
    for (int32_t field = 0; U_SUCCESS(status) && (field < UCAL_FIELD_COUNT); field++) {
        if (fStamp[field] >= kMinimumUserStamp) {
            validateField((UCalendarDateFields)field, status);
        }
    }
}

/**
* Validate a single field of this calendar.  Subclasses should
* override this method to validate any calendar-specific fields.
* Generic fields can be handled by
* <code>Calendar.validateField()</code>.
* @see #validateField(int, int, int)
*/
void Calendar::validateField(UCalendarDateFields field, UErrorCode &status) {
    int32_t y;
    switch (field) {
    case UCAL_DAY_OF_MONTH:
        y = handleGetExtendedYear();
        validateField(field, 1, handleGetMonthLength(y, internalGet(UCAL_MONTH)), status);
        break;
    case UCAL_DAY_OF_YEAR:
        y = handleGetExtendedYear();
        validateField(field, 1, handleGetYearLength(y), status);
        break;
    case UCAL_DAY_OF_WEEK_IN_MONTH:
        if (internalGet(field) == 0) {
#if defined (U_DEBUG_CAL)
            fprintf(stderr, "%s:%d: ILLEGAL ARG because DOW in month cannot be 0\n", 
                __FILE__, __LINE__);
#endif
            status = U_ILLEGAL_ARGUMENT_ERROR; // "DAY_OF_WEEK_IN_MONTH cannot be zero"
            return;
        }
        validateField(field, getMinimum(field), getMaximum(field), status);
        break;
    default:
        validateField(field, getMinimum(field), getMaximum(field), status);
        break;
    }
}

/**
* Validate a single field of this calendar given its minimum and
* maximum allowed value.  If the field is out of range, throw a
* descriptive <code>IllegalArgumentException</code>.  Subclasses may
* use this method in their implementation of {@link
* #validateField(int)}.
*/
void Calendar::validateField(UCalendarDateFields field, int32_t min, int32_t max, UErrorCode& status)
{
    int32_t value = fFields[field];
    if (value < min || value > max) {
#if defined (U_DEBUG_CAL)
        fprintf(stderr, "%s:%d: ILLEGAL ARG because of field %s out of range %d..%d  at %d\n", 
            __FILE__, __LINE__,fldName(field),min,max,value);
#endif
        status = U_ILLEGAL_ARGUMENT_ERROR;
        return;
    }
}

// -------------------------

const UFieldResolutionTable* Calendar::getFieldResolutionTable() const {
    return kDatePrecedence;
}


UCalendarDateFields Calendar::newerField(UCalendarDateFields defaultField, UCalendarDateFields alternateField) const
{
    if (fStamp[alternateField] > fStamp[defaultField]) {
        return alternateField;
    }
    return defaultField;
}

UCalendarDateFields Calendar::resolveFields(const UFieldResolutionTable* precedenceTable) {
    int32_t bestField = UCAL_FIELD_COUNT;
    int32_t tempBestField;
    UBool restoreWeekOfInternalSet = FALSE;
    if (fStamp[UCAL_DAY_OF_WEEK] >= kMinimumUserStamp &&
        fStamp[UCAL_DATE] >= kMinimumUserStamp &&
        fStamp[UCAL_MONTH] >= kMinimumUserStamp &&
        fStamp[UCAL_WEEK_OF_YEAR] == kInternallySet &&
        fStamp[UCAL_WEEK_OF_MONTH] == kInternallySet &&
        fStamp[UCAL_DAY_OF_WEEK_IN_MONTH] == kInternallySet) {
            int32_t monthStampDelta =  fStamp[UCAL_DAY_OF_WEEK] - fStamp[UCAL_MONTH];
            int32_t dateStampDelta =  fStamp[UCAL_DAY_OF_WEEK] - fStamp[UCAL_DATE];
            if ( monthStampDelta >= 1 && monthStampDelta <= 3 && dateStampDelta >= 1 && dateStampDelta <= 3 ) {
                // If UCAL_MONTH, UCAL_DATE and UCAL_DAY_OF_WEEK are all explicitly set nearly one after the
                // other (as when parsing a single date format), with UCAL_DAY_OF_WEEK set most recently, and
                // if UCAL_WEEK_OF_YEAR, UCAL_WEEK_OF_MONTH, and UCAL_DAY_OF_WEEK_IN_MONTH are all only
                // implicitly set (as from setTimeInMillis), then for the calculations in this call temporarily
                // treat UCAL_WEEK_OF_YEAR, UCAL_WEEK_OF_MONTH, and UCAL_DAY_OF_WEEK_IN_MONTH as unset so they
                // don't combine with UCAL_DAY_OF_WEEK to override the date in UCAL_MONTH & UCAL_DATE. All of
                // these conditions are to avoid messing up the case of parsing a format with UCAL_DAY_OF_WEEK
                // alone or in combination with other fields besides UCAL_MONTH, UCAL_DATE. Note: the current
                // stamp value is incremented each time Calendar::set is called to explicitly set a field value.
                fStamp[UCAL_WEEK_OF_YEAR] = kUnset;
                fStamp[UCAL_WEEK_OF_MONTH] = kUnset;
                fStamp[UCAL_DAY_OF_WEEK_IN_MONTH] = kUnset;
                restoreWeekOfInternalSet = TRUE;
            }
    }
    for (int32_t g=0; precedenceTable[g][0][0] != -1 && (bestField == UCAL_FIELD_COUNT); ++g) {
        int32_t bestStamp = kUnset;
        for (int32_t l=0; precedenceTable[g][l][0] != -1; ++l) {
            int32_t lineStamp = kUnset;
            // Skip over first entry if it is negative
            for (int32_t i=((precedenceTable[g][l][0]>=kResolveRemap)?1:0); precedenceTable[g][l][i]!=-1; ++i) {
                U_ASSERT(precedenceTable[g][l][i] < UCAL_FIELD_COUNT);
                int32_t s = fStamp[precedenceTable[g][l][i]];
                // If any field is unset then don't use this line
                if (s == kUnset) {
                    goto linesInGroup;
                } else if(s > lineStamp) {
                    lineStamp = s;
                }
            }
            // Record new maximum stamp & field no.
            if (lineStamp > bestStamp) {
                tempBestField = precedenceTable[g][l][0]; // First field refers to entire line
                if (tempBestField >= kResolveRemap) {
                    tempBestField &= (kResolveRemap-1);
                    // This check is needed to resolve some issues with UCAL_YEAR precedence mapping
                    if (tempBestField != UCAL_DATE || (fStamp[UCAL_WEEK_OF_MONTH] < fStamp[tempBestField])) {
                        bestField = tempBestField;
                    }
                } else {
                    bestField = tempBestField;
                }

                if (bestField == tempBestField) {
                    bestStamp = lineStamp;
                }
            }
linesInGroup:
            ;
        }
    }
    if (restoreWeekOfInternalSet) {
        // Restore the field stamps temporarily unset above.
        fStamp[UCAL_WEEK_OF_YEAR] = kInternallySet;
        fStamp[UCAL_WEEK_OF_MONTH] = kInternallySet;
        fStamp[UCAL_DAY_OF_WEEK_IN_MONTH] = kInternallySet;
    }
    return (UCalendarDateFields)bestField;
}

const UFieldResolutionTable Calendar::kDatePrecedence[] =
{ 
    {
        { UCAL_DAY_OF_MONTH, kResolveSTOP },
        { UCAL_WEEK_OF_YEAR, UCAL_DAY_OF_WEEK, kResolveSTOP },
        { UCAL_WEEK_OF_MONTH, UCAL_DAY_OF_WEEK, kResolveSTOP },
        { UCAL_DAY_OF_WEEK_IN_MONTH, UCAL_DAY_OF_WEEK, kResolveSTOP },
        { UCAL_WEEK_OF_YEAR, UCAL_DOW_LOCAL, kResolveSTOP },
        { UCAL_WEEK_OF_MONTH, UCAL_DOW_LOCAL, kResolveSTOP },
        { UCAL_DAY_OF_WEEK_IN_MONTH, UCAL_DOW_LOCAL, kResolveSTOP },
        { UCAL_DAY_OF_YEAR, kResolveSTOP },
        { kResolveRemap | UCAL_DAY_OF_MONTH, UCAL_YEAR, kResolveSTOP },  // if YEAR is set over YEAR_WOY use DAY_OF_MONTH
        { kResolveRemap | UCAL_WEEK_OF_YEAR, UCAL_YEAR_WOY, kResolveSTOP },  // if YEAR_WOY is set,  calc based on WEEK_OF_YEAR
        { kResolveSTOP }
    },
    {
        { UCAL_WEEK_OF_YEAR, kResolveSTOP },
        { UCAL_WEEK_OF_MONTH, kResolveSTOP },
        { UCAL_DAY_OF_WEEK_IN_MONTH, kResolveSTOP },
        { kResolveRemap | UCAL_DAY_OF_WEEK_IN_MONTH, UCAL_DAY_OF_WEEK, kResolveSTOP },
        { kResolveRemap | UCAL_DAY_OF_WEEK_IN_MONTH, UCAL_DOW_LOCAL, kResolveSTOP },
        { kResolveSTOP }
    }, 
    {{kResolveSTOP}}
};


const UFieldResolutionTable Calendar::kDOWPrecedence[] = 
{
    {
        { UCAL_DAY_OF_WEEK,kResolveSTOP, kResolveSTOP },
        { UCAL_DOW_LOCAL,kResolveSTOP, kResolveSTOP },
        {kResolveSTOP}
    },
    {{kResolveSTOP}}
};

// precedence for calculating a year
const UFieldResolutionTable Calendar::kYearPrecedence[] = 
{
    {
        { UCAL_YEAR, kResolveSTOP },
        { UCAL_EXTENDED_YEAR, kResolveSTOP },
        { UCAL_YEAR_WOY, UCAL_WEEK_OF_YEAR, kResolveSTOP },  // YEAR_WOY is useless without WEEK_OF_YEAR
        { kResolveSTOP }
    },
    {{kResolveSTOP}}
};


// -------------------------


void Calendar::computeTime(UErrorCode& status) {
    if (!isLenient()) {
        validateFields(status);
        if (U_FAILURE(status)) {
            return;
        }
    }

    // Compute the Julian day
    int32_t julianDay = computeJulianDay();

    double millis = Grego::julianDayToMillis(julianDay);

#if defined (U_DEBUG_CAL)
    //  int32_t julianInsanityCheck =  (int32_t)ClockMath::floorDivide(millis, kOneDay);
    //  julianInsanityCheck += kEpochStartAsJulianDay;
    //  if(1 || julianInsanityCheck != julianDay) {
    //    fprintf(stderr, "%s:%d- D'oh- computed jules %d, to mills (%s)%.lf, recomputed %d\n",
    //            __FILE__, __LINE__, julianDay, millis<0.0?"NEG":"", millis, julianInsanityCheck);
    //  }
#endif

    int32_t millisInDay;

    // We only use MILLISECONDS_IN_DAY if it has been set by the user.
    // This makes it possible for the caller to set the calendar to a
    // time and call clear(MONTH) to reset the MONTH to January.  This
    // is legacy behavior.  Without this, clear(MONTH) has no effect,
    // since the internally set JULIAN_DAY is used.
    if (fStamp[UCAL_MILLISECONDS_IN_DAY] >= ((int32_t)kMinimumUserStamp) &&
            newestStamp(UCAL_AM_PM, UCAL_MILLISECOND, kUnset) <= fStamp[UCAL_MILLISECONDS_IN_DAY]) {
        millisInDay = internalGet(UCAL_MILLISECONDS_IN_DAY);
    } else {
        millisInDay = computeMillisInDay();
    }

    UDate t = 0;
    if (fStamp[UCAL_ZONE_OFFSET] >= ((int32_t)kMinimumUserStamp) || fStamp[UCAL_DST_OFFSET] >= ((int32_t)kMinimumUserStamp)) {
        t = millis + millisInDay - (internalGet(UCAL_ZONE_OFFSET) + internalGet(UCAL_DST_OFFSET));
    } else {
        // Compute the time zone offset and DST offset.  There are two potential
        // ambiguities here.  We'll assume a 2:00 am (wall time) switchover time
        // for discussion purposes here.
        //
        // 1. The positive offset change such as transition into DST.
        //    Here, a designated time of 2:00 am - 2:59 am does not actually exist.
        //    For this case, skippedWallTime option specifies the behavior.
        //    For example, 2:30 am is interpreted as;
        //      - WALLTIME_LAST(default): 3:30 am (DST) (interpreting 2:30 am as 31 minutes after 1:59 am (STD))
        //      - WALLTIME_FIRST: 1:30 am (STD) (interpreting 2:30 am as 30 minutes before 3:00 am (DST))
        //      - WALLTIME_NEXT_VALID: 3:00 am (DST) (next valid time after 2:30 am on a wall clock)
        // 2. The negative offset change such as transition out of DST.
        //    Here, a designated time of 1:00 am - 1:59 am can be in standard or DST.  Both are valid
        //    representations (the rep jumps from 1:59:59 DST to 1:00:00 Std).
        //    For this case, repeatedWallTime option specifies the behavior.
        //    For example, 1:30 am is interpreted as;
        //      - WALLTIME_LAST(default): 1:30 am (STD) - latter occurrence
        //      - WALLTIME_FIRST: 1:30 am (DST) - former occurrence
        //
        // In addition to above, when calendar is strict (not default), wall time falls into
        // the skipped time range will be processed as an error case.
        //
        // These special cases are mostly handled in #computeZoneOffset(long), except WALLTIME_NEXT_VALID
        // at positive offset change. The protected method computeZoneOffset(long) is exposed to Calendar
        // subclass implementations and marked as @stable. Strictly speaking, WALLTIME_NEXT_VALID
        // should be also handled in the same place, but we cannot change the code flow without deprecating
        // the protected method.
        //
        // We use the TimeZone object, unless the user has explicitly set the ZONE_OFFSET
        // or DST_OFFSET fields; then we use those fields.

        if (!isLenient() || fSkippedWallTime == UCAL_WALLTIME_NEXT_VALID) {
            // When strict, invalidate a wall time falls into a skipped wall time range.
            // When lenient and skipped wall time option is WALLTIME_NEXT_VALID,
            // the result time will be adjusted to the next valid time (on wall clock).
            int32_t zoneOffset = computeZoneOffset(millis, millisInDay, status);
            UDate tmpTime = millis + millisInDay - zoneOffset;

            int32_t raw, dst;
            fZone->getOffset(tmpTime, FALSE, raw, dst, status);

            if (U_SUCCESS(status)) {
                // zoneOffset != (raw + dst) only when the given wall time fall into
                // a skipped wall time range caused by positive zone offset transition.
                if (zoneOffset != (raw + dst)) {
                    if (!isLenient()) {
                        status = U_ILLEGAL_ARGUMENT_ERROR;
                    } else {
                        U_ASSERT(fSkippedWallTime == UCAL_WALLTIME_NEXT_VALID);
                        // Adjust time to the next valid wall clock time.
                        // At this point, tmpTime is on or after the zone offset transition causing
                        // the skipped time range.
                        UDate immediatePrevTransition;
                        UBool hasTransition = getImmediatePreviousZoneTransition(tmpTime, &immediatePrevTransition, status);
                        if (U_SUCCESS(status) && hasTransition) {
                            t = immediatePrevTransition;
                        }
                    }
                } else {
                    t = tmpTime;
                }
            }
        } else {
            t = millis + millisInDay - computeZoneOffset(millis, millisInDay, status);
        }
    }
    if (U_SUCCESS(status)) {
        internalSetTime(t);
    }
}

/**
 * Find the previous zone transtion near the given time.
 */
UBool Calendar::getImmediatePreviousZoneTransition(UDate base, UDate *transitionTime, UErrorCode& status) const {
    BasicTimeZone *btz = getBasicTimeZone();
    if (btz) {
        TimeZoneTransition trans;
        UBool hasTransition = btz->getPreviousTransition(base, TRUE, trans);
        if (hasTransition) {
            *transitionTime = trans.getTime();
            return TRUE;
        } else {
            // Could not find any transitions.
            // Note: This should never happen.
            status = U_INTERNAL_PROGRAM_ERROR;
        }
    } else {
        // If not BasicTimeZone, return unsupported error for now.
        // TODO: We may support non-BasicTimeZone in future.
        status = U_UNSUPPORTED_ERROR;
    }
    return FALSE;
}

/**
* Compute the milliseconds in the day from the fields.  This is a
* value from 0 to 23:59:59.999 inclusive, unless fields are out of
* range, in which case it can be an arbitrary value.  This value
* reflects local zone wall time.
* @stable ICU 2.0
*/
int32_t Calendar::computeMillisInDay() {
  // Do the time portion of the conversion.

    int32_t millisInDay = 0;

    // Find the best set of fields specifying the time of day.  There
    // are only two possibilities here; the HOUR_OF_DAY or the
    // AM_PM and the HOUR.
    int32_t hourOfDayStamp = fStamp[UCAL_HOUR_OF_DAY];
    int32_t hourStamp = (fStamp[UCAL_HOUR] > fStamp[UCAL_AM_PM])?fStamp[UCAL_HOUR]:fStamp[UCAL_AM_PM];
    int32_t bestStamp = (hourStamp > hourOfDayStamp) ? hourStamp : hourOfDayStamp;

    // Hours
    if (bestStamp != kUnset) {
        if (bestStamp == hourOfDayStamp) {
            // Don't normalize here; let overflow bump into the next period.
            // This is consistent with how we handle other fields.
            millisInDay += internalGet(UCAL_HOUR_OF_DAY);
        } else {
            // Don't normalize here; let overflow bump into the next period.
            // This is consistent with how we handle other fields.
            millisInDay += internalGet(UCAL_HOUR);
            millisInDay += 12 * internalGet(UCAL_AM_PM); // Default works for unset AM_PM
        }
    }

    // We use the fact that unset == 0; we start with millisInDay
    // == HOUR_OF_DAY.
    millisInDay *= 60;
    millisInDay += internalGet(UCAL_MINUTE); // now have minutes
    millisInDay *= 60;
    millisInDay += internalGet(UCAL_SECOND); // now have seconds
    millisInDay *= 1000;
    millisInDay += internalGet(UCAL_MILLISECOND); // now have millis

    return millisInDay;
}

/**
* This method can assume EXTENDED_YEAR has been set.
* @param millis milliseconds of the date fields
* @param millisInDay milliseconds of the time fields; may be out
* or range.
* @stable ICU 2.0
*/
int32_t Calendar::computeZoneOffset(double millis, int32_t millisInDay, UErrorCode &ec) {
    int32_t rawOffset, dstOffset;
    UDate wall = millis + millisInDay;
    BasicTimeZone* btz = getBasicTimeZone();
    if (btz) {
        int duplicatedTimeOpt = (fRepeatedWallTime == UCAL_WALLTIME_FIRST) ? BasicTimeZone::kFormer : BasicTimeZone::kLatter;
        int nonExistingTimeOpt = (fSkippedWallTime == UCAL_WALLTIME_FIRST) ? BasicTimeZone::kLatter : BasicTimeZone::kFormer;
        btz->getOffsetFromLocal(wall, nonExistingTimeOpt, duplicatedTimeOpt, rawOffset, dstOffset, ec);
    } else {
        const TimeZone& tz = getTimeZone();
        // By default, TimeZone::getOffset behaves UCAL_WALLTIME_LAST for both.
        tz.getOffset(wall, TRUE, rawOffset, dstOffset, ec);

        UBool sawRecentNegativeShift = FALSE;
        if (fRepeatedWallTime == UCAL_WALLTIME_FIRST) {
            // Check if the given wall time falls into repeated time range
            UDate tgmt = wall - (rawOffset + dstOffset);

            // Any negative zone transition within last 6 hours?
            // Note: The maximum historic negative zone transition is -3 hours in the tz database.
            // 6 hour window would be sufficient for this purpose.
            int32_t tmpRaw, tmpDst;
            tz.getOffset(tgmt - 6*60*60*1000, FALSE, tmpRaw, tmpDst, ec);
            int32_t offsetDelta = (rawOffset + dstOffset) - (tmpRaw + tmpDst);

            U_ASSERT(offsetDelta < -6*60*60*1000);
            if (offsetDelta < 0) {
                sawRecentNegativeShift = TRUE;
                // Negative shift within last 6 hours. When UCAL_WALLTIME_FIRST is used and the given wall time falls
                // into the repeated time range, use offsets before the transition.
                // Note: If it does not fall into the repeated time range, offsets remain unchanged below.
                tz.getOffset(wall + offsetDelta, TRUE, rawOffset, dstOffset, ec);
            }
        }
        if (!sawRecentNegativeShift && fSkippedWallTime == UCAL_WALLTIME_FIRST) {
            // When skipped wall time option is WALLTIME_FIRST,
            // recalculate offsets from the resolved time (non-wall).
            // When the given wall time falls into skipped wall time,
            // the offsets will be based on the zone offsets AFTER
            // the transition (which means, earliest possibe interpretation).
            UDate tgmt = wall - (rawOffset + dstOffset);
            tz.getOffset(tgmt, FALSE, rawOffset, dstOffset, ec);
        }
    }
    return rawOffset + dstOffset;
}

int32_t Calendar::computeJulianDay() 
{
    // We want to see if any of the date fields is newer than the
    // JULIAN_DAY.  If not, then we use JULIAN_DAY.  If so, then we do
    // the normal resolution.  We only use JULIAN_DAY if it has been
    // set by the user.  This makes it possible for the caller to set
    // the calendar to a time and call clear(MONTH) to reset the MONTH
    // to January.  This is legacy behavior.  Without this,
    // clear(MONTH) has no effect, since the internally set JULIAN_DAY
    // is used.
    if (fStamp[UCAL_JULIAN_DAY] >= (int32_t)kMinimumUserStamp) {
        int32_t bestStamp = newestStamp(UCAL_ERA, UCAL_DAY_OF_WEEK_IN_MONTH, kUnset);
        bestStamp = newestStamp(UCAL_YEAR_WOY, UCAL_EXTENDED_YEAR, bestStamp);
        if (bestStamp <= fStamp[UCAL_JULIAN_DAY]) {
            return internalGet(UCAL_JULIAN_DAY);
        }
    }

    UCalendarDateFields bestField = resolveFields(getFieldResolutionTable());
    if (bestField == UCAL_FIELD_COUNT) {
        bestField = UCAL_DAY_OF_MONTH;
    }

    return handleComputeJulianDay(bestField);
}

// -------------------------------------------

int32_t Calendar::handleComputeJulianDay(UCalendarDateFields bestField)  {
    UBool useMonth = (bestField == UCAL_DAY_OF_MONTH ||
        bestField == UCAL_WEEK_OF_MONTH ||
        bestField == UCAL_DAY_OF_WEEK_IN_MONTH);
    int32_t year;

    if (bestField == UCAL_WEEK_OF_YEAR) {
        year = internalGet(UCAL_YEAR_WOY, handleGetExtendedYear());
        internalSet(UCAL_EXTENDED_YEAR, year);
    } else {
        year = handleGetExtendedYear();
        internalSet(UCAL_EXTENDED_YEAR, year);
    }

#if defined (U_DEBUG_CAL) 
    fprintf(stderr, "%s:%d: bestField= %s - y=%d\n", __FILE__, __LINE__, fldName(bestField), year);
#endif 

    // Get the Julian day of the day BEFORE the start of this year.
    // If useMonth is true, get the day before the start of the month.

    // give calendar subclass a chance to have a default 'first' month
    int32_t month;

    if(isSet(UCAL_MONTH)) {
        month = internalGet(UCAL_MONTH);
    } else {
        month = getDefaultMonthInYear(year);
    }

    int32_t julianDay = handleComputeMonthStart(year, useMonth ? month : 0, useMonth);

    if (bestField == UCAL_DAY_OF_MONTH) {

        // give calendar subclass a chance to have a default 'first' dom
        int32_t dayOfMonth;
        if(isSet(UCAL_DAY_OF_MONTH)) {
            dayOfMonth = internalGet(UCAL_DAY_OF_MONTH,1);
        } else {
            dayOfMonth = getDefaultDayInMonth(year, month);
        }
        return julianDay + dayOfMonth;
    }

    if (bestField == UCAL_DAY_OF_YEAR) {
        return julianDay + internalGet(UCAL_DAY_OF_YEAR);
    }

    int32_t firstDayOfWeek = getFirstDayOfWeek(); // Localized fdw

    // At this point julianDay is the 0-based day BEFORE the first day of
    // January 1, year 1 of the given calendar.  If julianDay == 0, it
    // specifies (Jan. 1, 1) - 1, in whatever calendar we are using (Julian
    // or Gregorian). (or it is before the month we are in, if useMonth is True)

    // At this point we need to process the WEEK_OF_MONTH or
    // WEEK_OF_YEAR, which are similar, or the DAY_OF_WEEK_IN_MONTH.
    // First, perform initial shared computations.  These locate the
    // first week of the period.

    // Get the 0-based localized DOW of day one of the month or year.
    // Valid range 0..6.
    int32_t first = julianDayToDayOfWeek(julianDay + 1) - firstDayOfWeek;
    if (first < 0) {
        first += 7;
    }

    int32_t dowLocal = getLocalDOW();

    // Find the first target DOW (dowLocal) in the month or year.
    // Actually, it may be just before the first of the month or year.
    // It will be an integer from -5..7.
    int32_t date = 1 - first + dowLocal;

    if (bestField == UCAL_DAY_OF_WEEK_IN_MONTH) {
        // Adjust the target DOW to be in the month or year.
        if (date < 1) {
            date += 7;
        }

        // The only trickiness occurs if the day-of-week-in-month is
        // negative.
        int32_t dim = internalGet(UCAL_DAY_OF_WEEK_IN_MONTH, 1);
        if (dim >= 0) {
            date += 7*(dim - 1);

        } else {
            // Move date to the last of this day-of-week in this month,
            // then back up as needed.  If dim==-1, we don't back up at
            // all.  If dim==-2, we back up once, etc.  Don't back up
            // past the first of the given day-of-week in this month.
            // Note that we handle -2, -3, etc. correctly, even though
            // values < -1 are technically disallowed.
            int32_t m = internalGet(UCAL_MONTH, UCAL_JANUARY);
            int32_t monthLength = handleGetMonthLength(year, m);
            date += ((monthLength - date) / 7 + dim + 1) * 7;
        }
    } else {
#if defined (U_DEBUG_CAL) 
        fprintf(stderr, "%s:%d - bf= %s\n", __FILE__, __LINE__, fldName(bestField));
#endif 

        if(bestField == UCAL_WEEK_OF_YEAR) {  // ------------------------------------- WOY -------------
            if(!isSet(UCAL_YEAR_WOY) ||  // YWOY not set at all or
                ( (resolveFields(kYearPrecedence) != UCAL_YEAR_WOY) // YWOY doesn't have precedence
                && (fStamp[UCAL_YEAR_WOY]!=kInternallySet) ) ) // (excluding where all fields are internally set - then YWOY is used)
            {
                // need to be sure to stay in 'real' year.
                int32_t woy = internalGet(bestField);

                int32_t nextJulianDay = handleComputeMonthStart(year+1, 0, FALSE); // jd of day before jan 1
                int32_t nextFirst = julianDayToDayOfWeek(nextJulianDay + 1) - firstDayOfWeek; 

                if (nextFirst < 0) { // 0..6 ldow of Jan 1
                    nextFirst += 7;
                }

                if(woy==1) {  // FIRST WEEK ---------------------------------
#if defined (U_DEBUG_CAL) 
                    fprintf(stderr, "%s:%d - woy=%d, yp=%d, nj(%d)=%d, nf=%d", __FILE__, __LINE__, 
                        internalGet(bestField), resolveFields(kYearPrecedence), year+1, 
                        nextJulianDay, nextFirst);

                    fprintf(stderr, " next: %d DFW,  min=%d   \n", (7-nextFirst), getMinimalDaysInFirstWeek() );
#endif 

                    // nextFirst is now the localized DOW of Jan 1  of y-woy+1
                    if((nextFirst > 0) &&   // Jan 1 starts on FDOW
                        (7-nextFirst) >= getMinimalDaysInFirstWeek()) // or enough days in the week
                    {
                        // Jan 1 of (yearWoy+1) is in yearWoy+1 - recalculate JD to next year
#if defined (U_DEBUG_CAL) 
                        fprintf(stderr, "%s:%d - was going to move JD from %d to %d [d%d]\n", __FILE__, __LINE__, 
                            julianDay, nextJulianDay, (nextJulianDay-julianDay));
#endif 
                        julianDay = nextJulianDay;

                        // recalculate 'first' [0-based local dow of jan 1]
                        first = julianDayToDayOfWeek(julianDay + 1) - firstDayOfWeek;
                        if (first < 0) {
                            first += 7;
                        }
                        // recalculate date.
                        date = 1 - first + dowLocal;
                    }
                } else if(woy>=getLeastMaximum(bestField)) {          
                    // could be in the last week- find out if this JD would overstep
                    int32_t testDate = date;
                    if ((7 - first) < getMinimalDaysInFirstWeek()) {
                        testDate += 7;
                    }

                    // Now adjust for the week number.
                    testDate += 7 * (woy - 1);

#if defined (U_DEBUG_CAL) 
                    fprintf(stderr, "%s:%d - y=%d, y-1=%d doy%d, njd%d (C.F. %d)\n",
                        __FILE__, __LINE__, year, year-1, testDate, julianDay+testDate, nextJulianDay);
#endif
                    if(julianDay+testDate > nextJulianDay) { // is it past Dec 31?  (nextJulianDay is day BEFORE year+1's  Jan 1)
                        // Fire up the calculating engines.. retry YWOY = (year-1)
                        julianDay = handleComputeMonthStart(year-1, 0, FALSE); // jd before Jan 1 of previous year
                        first = julianDayToDayOfWeek(julianDay + 1) - firstDayOfWeek; // 0 based local dow   of first week

                        if(first < 0) { // 0..6
                            first += 7;
                        }
                        date = 1 - first + dowLocal;

#if defined (U_DEBUG_CAL) 
                        fprintf(stderr, "%s:%d - date now %d, jd%d, ywoy%d\n",
                            __FILE__, __LINE__, date, julianDay, year-1);
#endif


                    } /* correction needed */
                } /* leastmaximum */
            } /* resolvefields(year) != year_woy */
        } /* bestfield != week_of_year */

        // assert(bestField == WEEK_OF_MONTH || bestField == WEEK_OF_YEAR)
        // Adjust for minimal days in first week
        if ((7 - first) < getMinimalDaysInFirstWeek()) {
            date += 7;
        }

        // Now adjust for the week number.
        date += 7 * (internalGet(bestField) - 1);
    }

    return julianDay + date;
}

int32_t
Calendar::getDefaultMonthInYear(int32_t /*eyear*/) 
{
    return 0;
}

int32_t
Calendar::getDefaultDayInMonth(int32_t /*eyear*/, int32_t /*month*/) 
{
    return 1;
}


int32_t Calendar::getLocalDOW()
{
  // Get zero-based localized DOW, valid range 0..6.  This is the DOW
    // we are looking for.
    int32_t dowLocal = 0;
    switch (resolveFields(kDOWPrecedence)) {
    case UCAL_DAY_OF_WEEK:
        dowLocal = internalGet(UCAL_DAY_OF_WEEK) - fFirstDayOfWeek;
        break;
    case UCAL_DOW_LOCAL:
        dowLocal = internalGet(UCAL_DOW_LOCAL) - 1;
        break;
    default:
        break;
    }
    dowLocal = dowLocal % 7;
    if (dowLocal < 0) {
        dowLocal += 7;
    }
    return dowLocal;
}

int32_t Calendar::handleGetExtendedYearFromWeekFields(int32_t yearWoy, int32_t woy)
{
    // We have UCAL_YEAR_WOY and UCAL_WEEK_OF_YEAR - from those, determine 
    // what year we fall in, so that other code can set it properly.
    // (code borrowed from computeWeekFields and handleComputeJulianDay)
    //return yearWoy;

    // First, we need a reliable DOW.
    UCalendarDateFields bestField = resolveFields(kDatePrecedence); // !! Note: if subclasses have a different table, they should override handleGetExtendedYearFromWeekFields 

    // Now, a local DOW
    int32_t dowLocal = getLocalDOW(); // 0..6
    int32_t firstDayOfWeek = getFirstDayOfWeek(); // Localized fdw
    int32_t jan1Start = handleComputeMonthStart(yearWoy, 0, FALSE);
    int32_t nextJan1Start = handleComputeMonthStart(yearWoy+1, 0, FALSE); // next year's Jan1 start

    // At this point julianDay is the 0-based day BEFORE the first day of
    // January 1, year 1 of the given calendar.  If julianDay == 0, it
    // specifies (Jan. 1, 1) - 1, in whatever calendar we are using (Julian
    // or Gregorian). (or it is before the month we are in, if useMonth is True)

    // At this point we need to process the WEEK_OF_MONTH or
    // WEEK_OF_YEAR, which are similar, or the DAY_OF_WEEK_IN_MONTH.
    // First, perform initial shared computations.  These locate the
    // first week of the period.

    // Get the 0-based localized DOW of day one of the month or year.
    // Valid range 0..6.
    int32_t first = julianDayToDayOfWeek(jan1Start + 1) - firstDayOfWeek;
    if (first < 0) {
        first += 7;
    }
    int32_t nextFirst = julianDayToDayOfWeek(nextJan1Start + 1) - firstDayOfWeek;
    if (nextFirst < 0) {
        nextFirst += 7;
    }

    int32_t minDays = getMinimalDaysInFirstWeek();
    UBool jan1InPrevYear = FALSE;  // January 1st in the year of WOY is the 1st week?  (i.e. first week is < minimal )
    //UBool nextJan1InPrevYear = FALSE; // January 1st of Year of WOY + 1 is in the first week? 

    if((7 - first) < minDays) { 
        jan1InPrevYear = TRUE;
    }

    //   if((7 - nextFirst) < minDays) {
    //     nextJan1InPrevYear = TRUE;
    //   }

    switch(bestField) {
    case UCAL_WEEK_OF_YEAR:
        if(woy == 1) {
            if(jan1InPrevYear == TRUE) {
                // the first week of January is in the previous year
                // therefore WOY1 is always solidly within yearWoy
                return yearWoy;
            } else {
                // First WOY is split between two years
                if( dowLocal < first) { // we are prior to Jan 1
                    return yearWoy-1; // previous year
                } else {
                    return yearWoy; // in this year
                }
            }
        } else if(woy >= getLeastMaximum(bestField)) {  
            // we _might_ be in the last week.. 
            int32_t jd =  // Calculate JD of our target day:
                jan1Start +  // JD of Jan 1
                (7-first) + //  days in the first week (Jan 1.. )
                (woy-1)*7 + // add the weeks of the year
                dowLocal;   // the local dow (0..6) of last week
            if(jan1InPrevYear==FALSE) {
                jd -= 7; // woy already includes Jan 1's week.
            }

            if( (jd+1) >= nextJan1Start ) {
                // we are in week 52 or 53 etc. - actual year is yearWoy+1
                return yearWoy+1;
            } else {
                // still in yearWoy;
                return yearWoy;
            }
        } else {
            // we're not possibly in the last week -must be ywoy
            return yearWoy;
        }

    case UCAL_DATE:
        if((internalGet(UCAL_MONTH)==0) &&
            (woy >= getLeastMaximum(UCAL_WEEK_OF_YEAR))) {
                return yearWoy+1; // month 0, late woy = in the next year
            } else if(woy==1) {
                //if(nextJan1InPrevYear) {
                if(internalGet(UCAL_MONTH)==0) {
                    return yearWoy;
                } else {
                    return yearWoy-1;
                }
                //}
            }

            //(internalGet(UCAL_DATE) <= (7-first)) /* && in minDow  */ ) {
            //within 1st week and in this month.. 
            //return yearWoy+1;
            return yearWoy;

    default: // assume the year is appropriate
        return yearWoy;
    }
}

int32_t Calendar::handleGetMonthLength(int32_t extendedYear, int32_t month) const
{
    return handleComputeMonthStart(extendedYear, month+1, TRUE) -
        handleComputeMonthStart(extendedYear, month, TRUE);
}

int32_t Calendar::handleGetYearLength(int32_t eyear) const  {
    return handleComputeMonthStart(eyear+1, 0, FALSE) -
        handleComputeMonthStart(eyear, 0, FALSE);
}

int32_t
Calendar::getActualMaximum(UCalendarDateFields field, UErrorCode& status) const
{
    int32_t result;
    switch (field) {
    case UCAL_DATE:
        {
            if(U_FAILURE(status)) return 0;
            Calendar *cal = clone();
            if(!cal) { status = U_MEMORY_ALLOCATION_ERROR; return 0; }
            cal->setLenient(TRUE);
            cal->prepareGetActual(field,FALSE,status);
            result = handleGetMonthLength(cal->get(UCAL_EXTENDED_YEAR, status), cal->get(UCAL_MONTH, status));
            delete cal;
        }
        break;

    case UCAL_DAY_OF_YEAR:
        {
            if(U_FAILURE(status)) return 0;
            Calendar *cal = clone();
            if(!cal) { status = U_MEMORY_ALLOCATION_ERROR; return 0; }
            cal->setLenient(TRUE);
            cal->prepareGetActual(field,FALSE,status);
            result = handleGetYearLength(cal->get(UCAL_EXTENDED_YEAR, status));
            delete cal;
        }
        break;

    case UCAL_DAY_OF_WEEK:
    case UCAL_AM_PM:
    case UCAL_HOUR:
    case UCAL_HOUR_OF_DAY:
    case UCAL_MINUTE:
    case UCAL_SECOND:
    case UCAL_MILLISECOND:
    case UCAL_ZONE_OFFSET:
    case UCAL_DST_OFFSET:
    case UCAL_DOW_LOCAL:
    case UCAL_JULIAN_DAY:
    case UCAL_MILLISECONDS_IN_DAY:
        // These fields all have fixed minima/maxima
        result = getMaximum(field);
        break;

    default:
        // For all other fields, do it the hard way....
        result = getActualHelper(field, getLeastMaximum(field), getMaximum(field),status);
        break;
    }
    return result;
}


/**
* Prepare this calendar for computing the actual minimum or maximum.
* This method modifies this calendar's fields; it is called on a
* temporary calendar.
*
* <p>Rationale: The semantics of getActualXxx() is to return the
* maximum or minimum value that the given field can take, taking into
* account other relevant fields.  In general these other fields are
* larger fields.  For example, when computing the actual maximum
* DATE, the current value of DATE itself is ignored,
* as is the value of any field smaller.
*
* <p>The time fields all have fixed minima and maxima, so we don't
* need to worry about them.  This also lets us set the
* MILLISECONDS_IN_DAY to zero to erase any effects the time fields
* might have when computing date fields.
*
* <p>DAY_OF_WEEK is adjusted specially for the WEEK_OF_MONTH and
* WEEK_OF_YEAR fields to ensure that they are computed correctly.
* @internal
*/
void Calendar::prepareGetActual(UCalendarDateFields field, UBool isMinimum, UErrorCode &status)
{
    set(UCAL_MILLISECONDS_IN_DAY, 0);

    switch (field) {
    case UCAL_YEAR:
    case UCAL_EXTENDED_YEAR:
        set(UCAL_DAY_OF_YEAR, getGreatestMinimum(UCAL_DAY_OF_YEAR));
        break;

    case UCAL_YEAR_WOY:
        set(UCAL_WEEK_OF_YEAR, getGreatestMinimum(UCAL_WEEK_OF_YEAR));

    case UCAL_MONTH:
        set(UCAL_DATE, getGreatestMinimum(UCAL_DATE));
        break;

    case UCAL_DAY_OF_WEEK_IN_MONTH:
        // For dowim, the maximum occurs for the DOW of the first of the
        // month.
        set(UCAL_DATE, 1);
        set(UCAL_DAY_OF_WEEK, get(UCAL_DAY_OF_WEEK, status)); // Make this user set
        break;

    case UCAL_WEEK_OF_MONTH:
    case UCAL_WEEK_OF_YEAR:
        // If we're counting weeks, set the day of the week to either the
        // first or last localized DOW.  We know the last week of a month
        // or year will contain the first day of the week, and that the
        // first week will contain the last DOW.
        {
            int32_t dow = fFirstDayOfWeek;
            if (isMinimum) {
                dow = (dow + 6) % 7; // set to last DOW
                if (dow < UCAL_SUNDAY) {
                    dow += 7;
                }
            }
#if defined (U_DEBUG_CAL) 
            fprintf(stderr, "prepareGetActualHelper(WOM/WOY) - dow=%d\n", dow);
#endif
            set(UCAL_DAY_OF_WEEK, dow);
        }
        break;
    default:
        break;
    }

    // Do this last to give it the newest time stamp
    set(field, getGreatestMinimum(field));
}

int32_t Calendar::getActualHelper(UCalendarDateFields field, int32_t startValue, int32_t endValue, UErrorCode &status) const
{
#if defined (U_DEBUG_CAL) 
    fprintf(stderr, "getActualHelper(%d,%d .. %d, %s)\n", field, startValue, endValue, u_errorName(status));
#endif
    if (startValue == endValue) {
        // if we know that the maximum value is always the same, just return it
        return startValue;
    }

    int32_t delta = (endValue > startValue) ? 1 : -1;

    // clone the calendar so we don't mess with the real one, and set it to
    // accept anything for the field values
    if(U_FAILURE(status)) return startValue;
    Calendar *work = clone();
    if(!work) { status = U_MEMORY_ALLOCATION_ERROR; return startValue; }

    // need to resolve time here, otherwise, fields set for actual limit
    // may cause conflict with fields previously set (but not yet resolved).
    work->complete(status);

    work->setLenient(TRUE);
    work->prepareGetActual(field, delta < 0, status);

    // now try each value from the start to the end one by one until
    // we get a value that normalizes to another value.  The last value that
    // normalizes to itself is the actual maximum for the current date
    work->set(field, startValue);

    // prepareGetActual sets the first day of week in the same week with
    // the first day of a month.  Unlike WEEK_OF_YEAR, week number for the
    // week which contains days from both previous and current month is
    // not unique.  For example, last several days in the previous month
    // is week 5, and the rest of week is week 1.
    int32_t result = startValue;
    if ((work->get(field, status) != startValue
         && field != UCAL_WEEK_OF_MONTH && delta > 0 ) || U_FAILURE(status)) {
#if defined (U_DEBUG_CAL) 
        fprintf(stderr, "getActualHelper(fld %d) - got  %d (not %d) - %s\n", field, work->get(field,status), startValue, u_errorName(status));
#endif
    } else {
        do {
            startValue += delta;
            work->add(field, delta, status);
            if (work->get(field, status) != startValue || U_FAILURE(status)) {
#if defined (U_DEBUG_CAL)
                fprintf(stderr, "getActualHelper(fld %d) - got  %d (not %d), BREAK - %s\n", field, work->get(field,status), startValue, u_errorName(status));
#endif
                break;
            }
            result = startValue;
        } while (startValue != endValue);
    }
    delete work;
#if defined (U_DEBUG_CAL) 
    fprintf(stderr, "getActualHelper(%d) = %d\n", field, result);
#endif
    return result;
}




// -------------------------------------

void
Calendar::setWeekData(const Locale& desiredLocale, const char *type, UErrorCode& status)
{

    if (U_FAILURE(status)) return;

    fFirstDayOfWeek = UCAL_SUNDAY;
    fMinimalDaysInFirstWeek = 1;
    fWeekendOnset = UCAL_SATURDAY;
    fWeekendOnsetMillis = 0;
    fWeekendCease = UCAL_SUNDAY;
    fWeekendCeaseMillis = 86400000; // 24*60*60*1000

    // Since week and weekend data is territory based instead of language based,
    // we may need to tweak the locale that we are using to try to get the appropriate
    // values, using the following logic:
    // 1). If the locale has a language but no territory, use the territory as defined by 
    //     the likely subtags.
    // 2). If the locale has a script designation then we ignore it,
    //     then remove it ( i.e. "en_Latn_US" becomes "en_US" )
 
    char minLocaleID[ULOC_FULLNAME_CAPACITY] = { 0 };
    UErrorCode myStatus = U_ZERO_ERROR;

    uloc_minimizeSubtags(desiredLocale.getName(),minLocaleID,ULOC_FULLNAME_CAPACITY,&myStatus);
    Locale min = Locale::createFromName(minLocaleID);
    Locale useLocale;
    if ( uprv_strlen(desiredLocale.getCountry()) == 0 || 
         (uprv_strlen(desiredLocale.getScript()) > 0 && uprv_strlen(min.getScript()) == 0) ) {
        char maxLocaleID[ULOC_FULLNAME_CAPACITY] = { 0 };
        myStatus = U_ZERO_ERROR;
        uloc_addLikelySubtags(desiredLocale.getName(),maxLocaleID,ULOC_FULLNAME_CAPACITY,&myStatus);
        Locale max = Locale::createFromName(maxLocaleID);
        useLocale = Locale(max.getLanguage(),max.getCountry());
    } else {
        useLocale = Locale(desiredLocale);
    }
 
    /* The code here is somewhat of a hack, since week data and weekend data aren't really tied to 
       a specific calendar, they aren't truly locale data.  But this is the only place where valid and
       actual locale can be set, so we take a shot at it here by loading a representative resource
       from the calendar data.  The code used to use the dateTimeElements resource to get first day
       of week data, but this was moved to supplemental data under ticket 7755. (JCE) */

    CalendarData calData(useLocale,type,status);
    UResourceBundle *monthNames = calData.getByKey(gMonthNames,status);
    if (U_SUCCESS(status)) {
        U_LOCALE_BASED(locBased,*this);
        locBased.setLocaleIDs(ures_getLocaleByType(monthNames, ULOC_VALID_LOCALE, &status),
                              ures_getLocaleByType(monthNames, ULOC_ACTUAL_LOCALE, &status));
    } else {
        status = U_USING_FALLBACK_WARNING;
        return;
    }

    
    // Read week data values from supplementalData week data
    UResourceBundle *rb = ures_openDirect(NULL, "supplementalData", &status);
    ures_getByKey(rb, "weekData", rb, &status);
    UResourceBundle *weekData = ures_getByKey(rb, useLocale.getCountry(), NULL, &status);
    if (status == U_MISSING_RESOURCE_ERROR && rb != NULL) {
        status = U_ZERO_ERROR;
        weekData = ures_getByKey(rb, "001", NULL, &status);
    }

    if (U_FAILURE(status)) {
#if defined (U_DEBUG_CALDATA)
        fprintf(stderr, " Failure loading weekData from supplemental = %s\n", u_errorName(status));
#endif
        status = U_USING_FALLBACK_WARNING;
    } else {
        int32_t arrLen;
        const int32_t *weekDataArr = ures_getIntVector(weekData,&arrLen,&status);
        if( U_SUCCESS(status) && arrLen == 6
                && 1 <= weekDataArr[0] && weekDataArr[0] <= 7
                && 1 <= weekDataArr[1] && weekDataArr[1] <= 7
                && 1 <= weekDataArr[2] && weekDataArr[2] <= 7
                && 1 <= weekDataArr[4] && weekDataArr[4] <= 7) {
            fFirstDayOfWeek = (UCalendarDaysOfWeek)weekDataArr[0];
            fMinimalDaysInFirstWeek = (uint8_t)weekDataArr[1];
            fWeekendOnset = (UCalendarDaysOfWeek)weekDataArr[2];
            fWeekendOnsetMillis = weekDataArr[3];
            fWeekendCease = (UCalendarDaysOfWeek)weekDataArr[4];
            fWeekendCeaseMillis = weekDataArr[5];
        } else {
            status = U_INVALID_FORMAT_ERROR;
        }
    }
    ures_close(weekData);
    ures_close(rb);
}

/**
* Recompute the time and update the status fields isTimeSet
* and areFieldsSet.  Callers should check isTimeSet and only
* call this method if isTimeSet is false.
*/
void 
Calendar::updateTime(UErrorCode& status) 
{
    computeTime(status);
    if(U_FAILURE(status))
        return;

    // If we are lenient, we need to recompute the fields to normalize
    // the values.  Also, if we haven't set all the fields yet (i.e.,
    // in a newly-created object), we need to fill in the fields. [LIU]
    if (isLenient() || ! fAreAllFieldsSet) 
        fAreFieldsSet = FALSE;

    fIsTimeSet = TRUE;
    fAreFieldsVirtuallySet = FALSE;
}

Locale 
Calendar::getLocale(ULocDataLocaleType type, UErrorCode& status) const {
    U_LOCALE_BASED(locBased, *this);
    return locBased.getLocale(type, status);
}

const char *
Calendar::getLocaleID(ULocDataLocaleType type, UErrorCode& status) const {
    U_LOCALE_BASED(locBased, *this);
    return locBased.getLocaleID(type, status);
}

void
Calendar::recalculateStamp() {
    int32_t index;
    int32_t currentValue;
    int32_t j, i;

    fNextStamp = 1;

    for (j = 0; j < UCAL_FIELD_COUNT; j++) {
        currentValue = STAMP_MAX;
        index = -1;
        for (i = 0; i < UCAL_FIELD_COUNT; i++) {
            if (fStamp[i] > fNextStamp && fStamp[i] < currentValue) {
                currentValue = fStamp[i];
                index = i;
            }
        }

        if (index >= 0) {
            fStamp[index] = ++fNextStamp;
        } else {
            break;
        }
    }
    fNextStamp++;
}

// Deprecated function. This doesn't need to be inline.
void
Calendar::internalSet(EDateFields field, int32_t value)
{
    internalSet((UCalendarDateFields) field, value);
}

BasicTimeZone*
Calendar::getBasicTimeZone(void) const {
    if (dynamic_cast<const OlsonTimeZone *>(fZone) != NULL
        || dynamic_cast<const SimpleTimeZone *>(fZone) != NULL
        || dynamic_cast<const RuleBasedTimeZone *>(fZone) != NULL
        || dynamic_cast<const VTimeZone *>(fZone) != NULL) {
        return (BasicTimeZone*)fZone;
    }
    return NULL;
}

U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_FORMATTING */


//eof
