// © 2022 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING

#include "iso8601cal.h"
#include "unicode/gregocal.h"

U_NAMESPACE_BEGIN

UOBJECT_DEFINE_RTTI_IMPLEMENTATION(ISO8601Calendar)

ISO8601Calendar::ISO8601Calendar(const Locale& aLocale, UErrorCode& success)
:   GregorianCalendar(aLocale, success)
{
    setFirstDayOfWeek(UCAL_MONDAY);
    setMinimalDaysInFirstWeek(4);
}

ISO8601Calendar::~ISO8601Calendar()
{
}

ISO8601Calendar* ISO8601Calendar::clone() const
{
    return new ISO8601Calendar(*this);
}

const char *ISO8601Calendar::getType() const
{
    return "iso8601";
}

U_NAMESPACE_END

#endif
