#ifndef lint
#ifndef NOID
static char	elsieid[] = "%W%";
#endif /* !defined NOID */
#endif /* !defined lint */

/*
** Leap second handling from Bradley White (bww@k.gp.cs.cmu.edu).
** POSIX-format TZ environment variable handling from Guy Harris
** (guy@auspex.uucp).
*/

/*LINTLIBRARY*/

#include "tzfile.h"
#include "time.h"
#include "string.h"
#include "ctype.h"
#include "stdlib.h"
#include "stdio.h"	/* for FILENAME_MAX */
#include "fcntl.h"	/* for O_RDONLY */
#include "nonstd.h"

#ifdef __TURBOC__
#include "io.h"		/* for open et al. prototypes */
#endif /* defined __TURBOC__ */

#define ACCESS_MODE	O_RDONLY

#ifdef O_BINARY
#define OPEN_MODE	O_RDONLY | O_BINARY
#else /* !defined O_BINARY */
#define OPEN_MODE	O_RDONLY
#endif /* !defined O_BINARY */

#ifndef TRUE
#define TRUE		1
#define FALSE		0
#endif /* !defined TRUE */

static long		detzcode P((const char * codep));
static void		settzname P((const struct state *sp));
static char *		getzname P((const char *strp));
static char *		getnum P((const char *strp, int *nump, int min,
				int max));
static char *		gettime P((const char *strp, long *timep));
static char *		getoffset P((const char *strp, long *offsetp));
static char *		getrule P((const char *strp, struct rule *rulep));
static time_t		transtime P((time_t janfirst, int year,
				const struct rule *rulep, long offset));
static int		tzparse P((const char *name, struct state *sp));
#ifdef STD_INSPIRED
struct tm *		offtime P((const time_t * clockp, long offset));
#endif /* !defined STD_INSPIRED */
static void		timesub P((const time_t * clockp, long offset,
				const struct state * sp, struct tm * tmp));
static int		tzload P((const char * name, struct state * sp));
void			tzsetwall P((void));

struct ttinfo {				/* time type information */
	long		tt_gmtoff;	/* GMT offset in seconds */
	int		tt_isdst;	/* used to set tm_isdst */
	int		tt_abbrind;	/* abbreviation list index */
};

struct lsinfo {				/* leap second information */
	time_t		ls_trans;	/* transition time */
	long		ls_corr;	/* correction to apply */
};

struct state {
	int		leapcnt;
	int		timecnt;
	int		typecnt;
	int		charcnt;
	time_t		ats[TZ_MAX_TIMES];
	unsigned char	types[TZ_MAX_TIMES];
	struct ttinfo	ttis[TZ_MAX_TYPES];
	char		chars[TZ_MAX_CHARS + 1];
	struct lsinfo	lsis[TZ_MAX_LEAPS];
};

static struct state	lclstate;
static struct state	gmtstate;

static int		lcl_is_set;
static int		gmt_is_set;

char *			tzname[2] = {
	"GMT",
	"GMT"
};

#ifdef USG_COMPAT
time_t			timezone = 0;
int			daylight = 0;
#endif /* defined USG_COMPAT */

#ifdef TZA_COMPAT
char *			tz_abbr;	/* compatibility w/older versions */
#endif /* defined TZA_COMPAT */

static long
detzcode(codep)
const char *	codep;
{
	register long	result;
	register int	i;

	result = 0;
	for (i = 0; i < 4; ++i)
		result = (result << 8) | (codep[i] & 0xff);
	return result;
}

static void
settzname(sp)
register const struct state *	sp;
{
	register int	i;

	tzname[0] = tzname[1] = &sp->chars[0];
#ifdef USG_COMPAT
	timezone = -sp->ttis[0].tt_gmtoff;
	daylight = 0;
#endif /* defined USG_COMPAT */
	for (i = 1; i < sp->typecnt; ++i) {
		register const struct ttinfo *	ttisp;

		ttisp = &sp->ttis[i];
		if (ttisp->tt_isdst) {
			tzname[1] = &sp->chars[ttisp->tt_abbrind];
#ifdef USG_COMPAT
			daylight = 1;
#endif /* defined USG_COMPAT */
		} else {
			tzname[0] = &sp->chars[ttisp->tt_abbrind];
#ifdef USG_COMPAT
			timezone = -ttisp->tt_gmtoff;
#endif /* defined USG_COMPAT */
		}
	}
}

static int
tzload(name, sp)
register const char *	name;
register struct state *	sp;
{
	register const char *	p;
	register int		i;
	register int		fid;

	if (name == 0 && (name = TZDEFAULT) == 0)
		return -1;
	{
		register int 	doaccess;
		char		fullname[FILENAME_MAX + 1];

		if (name[0] == ':')
			name++;
		doaccess = name[0] == '/';
		if (!doaccess) {
			if ((p = TZDIR) == NULL)
				return -1;
			if ((strlen(p) + strlen(name) + 1) >= sizeof fullname)
				return -1;
			(void) strcpy(fullname, p);
			(void) strcat(fullname, "/");
			(void) strcat(fullname, name);
			/*
			** Set doaccess if '.' (as in "../") shows up in name.
			*/
			if (strchr(name, '.') != NULL)
				doaccess = TRUE;
			name = fullname;
		}
		if (doaccess && access(name, ACCESS_MODE) != 0)
			return -1;
		if ((fid = open(name, OPEN_MODE)) == -1)
			return -1;
	}
	{
		register const struct tzhead *	tzhp;
		char				buf[sizeof *sp];

		i = read(fid, buf, sizeof buf);
		if (close(fid) != 0 || i < sizeof *tzhp)
			return -1;
		tzhp = (struct tzhead *) buf;
		sp->leapcnt = (int) detzcode(tzhp->tzh_leapcnt);
		sp->timecnt = (int) detzcode(tzhp->tzh_timecnt);
		sp->typecnt = (int) detzcode(tzhp->tzh_typecnt);
		sp->charcnt = (int) detzcode(tzhp->tzh_charcnt);
		if (sp->leapcnt > TZ_MAX_LEAPS ||
			sp->timecnt > TZ_MAX_TIMES ||
			sp->typecnt == 0 ||
			sp->typecnt > TZ_MAX_TYPES ||
			sp->charcnt > TZ_MAX_CHARS)
				return -1;
		if (i < sizeof *tzhp +
			sp->timecnt * (4 + sizeof (char)) +
			sp->typecnt * (4 + 2 * sizeof (char)) +
			sp->charcnt * sizeof (char) +
			sp->leapcnt * 2 * 4)
				return -1;
		p = buf + sizeof *tzhp;
		for (i = 0; i < sp->timecnt; ++i) {
			sp->ats[i] = detzcode(p);
			p += 4;
		}
		for (i = 0; i < sp->timecnt; ++i)
			sp->types[i] = (unsigned char) *p++;
		for (i = 0; i < sp->typecnt; ++i) {
			register struct ttinfo *	ttisp;

			ttisp = &sp->ttis[i];
			ttisp->tt_gmtoff = detzcode(p);
			p += 4;
			ttisp->tt_isdst = (unsigned char) *p++;
			ttisp->tt_abbrind = (unsigned char) *p++;
		}
		for (i = 0; i < sp->charcnt; ++i)
			sp->chars[i] = *p++;
		sp->chars[i] = '\0';	/* ensure '\0' at end */
		for (i = 0; i < sp->leapcnt; ++i) {
			register struct lsinfo *	lsisp;

			lsisp = &sp->lsis[i];
			lsisp->ls_trans = detzcode(p);
			p += 4;
			lsisp->ls_corr = detzcode(p);
			p += 4;
		}
	}
	/*
	** Check that all the local time type indices are valid.
	*/
	for (i = 0; i < sp->timecnt; ++i)
		if (sp->types[i] >= sp->typecnt)
			return -1;
	/*
	** Check that all abbreviation indices are valid.
	*/
	for (i = 0; i < sp->typecnt; ++i)
		if (sp->ttis[i].tt_abbrind >= sp->charcnt)
			return -1;
	/*
	** Set tzname elements to initial values.
	*/
	if (sp == &lclstate)
		settzname(sp);
	return 0;
}

struct rule {
	int	r_type;		/* type of rule */
	int	r_day;		/* day number of rule */
	int	r_week;		/* week number of rule */
	int	r_mon;		/* month number of rule */
	long	r_time;		/* transition time of rule */
};

#define	JULIAN_DAY		0	/* Jn - Julian day */
#define	DAY_OF_YEAR		1	/* n - day of year */
#define	MONTH_NTH_DAY_OF_WEEK	2	/* Mm.n.d - month, week, day of week */
#ifdef BROKEN
#define	MONTH_DAY		3	/* Mm.n.d - broken POSIX style */
#endif

static const int	mon_lengths[2][MONSPERYEAR] = {
	31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
	31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static const int	year_lengths[2] = {
	DAYSPERNYEAR, DAYSPERLYEAR
};

/*
** Given a pointer into a time zone string, scan until a character that is not
** a valid character in a zone name is found.  Return a pointer to that
** character.
*/
static char *
getzname(strp)
register const char *	strp;
{
	register char	c;

	while ((c = *strp) != '\0' && !isdigit(c) && c != ',' && c != '-' &&
		c != '+')
			++strp;
	return strp;
}

/*
** Given a pointer into a time zone string, extract a number from that string.
** Check that the number is within a specified range; if it is not, return
** NULL.
** Otherwise, return a pointer to the first character not part of the number.
*/
static char *
getnum(strp, nump, min, max)
register const char *	strp;
int *			nump;
int			min;
int			max;
{
	register char	c;
	register int	num;

	num = 0;
	while ((c = *strp) != '\0' && isdigit(c)) {
		num = num * 10 + (c - '0');
		if (num > max)
			return NULL;	/* illegal value */
		++strp;
	}
	if (num < min)
		return NULL;		/* illegal value */
	*nump = num;
	return strp;
}

/*
** Given a pointer into a time zone string, extract a time, in hh[:mm[:ss]]
** form, from the string.
** If any error occurs, return NULL.
** Otherwise, return a pointer to the first character not part of the time.
*/
static char *
gettime(strp, timep)
register const char *	strp;
long *			timep;
{
	int	num;

	strp = getnum(strp, &num, 0, HOURSPERDAY / 2);
	if (strp == NULL)
		return NULL;
	*timep = num * SECSPERHOUR;
	if (*strp == ':') {
		++strp;
		strp = getnum(strp, &num, 0, MINSPERHOUR - 1);
		if (strp == NULL)
			return NULL;
		*timep += num * SECSPERMIN;
		if (*strp == ':') {
			++strp;
			strp = getnum(strp, &num, 0, SECSPERMIN - 1);
			if (strp == NULL)
				return NULL;
			*timep += num;
		}
	}
	return strp;
}

/*
** Given a pointer into a time zone string, extract an offset, in
** [+-]hh[:mm[:ss]] form, from the string.
** If any error occurs, return NULL.
** Otherwise, return a pointer to the first character not part of the time.
*/
static char *
getoffset(strp, offsetp)
register const char *	strp;
long *			offsetp;
{
	register int	neg;

	if (*strp == '-') {
		neg = 1;
		++strp;
	} else if (*strp == '+' || isdigit(*strp))
		neg = 0;
	else	return NULL;		/* illegal offset */
	strp = gettime(strp, offsetp);
	if (strp == NULL)
		return NULL;		/* illegal time */
	if (neg)
		*offsetp = -*offsetp;
	return strp;
}

/*
** Given a pointer into a time zone string, extract a rule in the form
** date[/time].  See POSIX section 8 for the format of "date" and "time".
** If a valid rule is not found, return NULL.
** Otherwise, return a pointer to the first character not part of the rule.
*/
static char *
getrule(strp, rulep)
const char *		strp;
register struct rule *	rulep;
{
	if (*strp == 'J') {
		/*
		** Julian day.
		*/
		rulep->r_type = JULIAN_DAY;
		++strp;
		strp = getnum(strp, &rulep->r_day, 1, DAYSPERNYEAR);
	} else if (*strp == 'M') {
		/*
		** Month, week, day.
		*/
#ifdef BROKEN
		rulep->r_type = MONTH_DAY;
#else
		rulep->r_type = MONTH_NTH_DAY_OF_WEEK;
#endif
		++strp;
		strp = getnum(strp, &rulep->r_mon, 1, MONSPERYEAR);
		if (strp == NULL)
			return NULL;
		if (*strp++ != '.')
			return NULL;
		strp = getnum(strp, &rulep->r_week, 1, 5);
		if (strp == NULL)
			return NULL;
		if (*strp++ != '.')
			return NULL;
		strp = getnum(strp, &rulep->r_day, 0, DAYSPERWEEK - 1);
	} else if (isdigit(*strp)) {
		/*
		** Day of year.
		*/
		rulep->r_type = DAY_OF_YEAR;
		strp = getnum(strp, &rulep->r_day, 0, DAYSPERLYEAR - 1);
	} else	return NULL;		/* invalid format */
	if (strp == NULL)
		return NULL;
	if (*strp == '/') {
		/*
		** Time specified.
		*/
		++strp;
		strp = gettime(strp, &rulep->r_time);
		if (strp == NULL)
			return NULL;
	} else	rulep->r_time = 2 * SECSPERHOUR;	/* default = 2:00:00 */
	return strp;
}

/*
** Given the Epoch-relative time of January 1, 00:00:00 GMT, in a year, the
** year, a rule, and the offset from GMT at the time that rule takes effect,
** calculate the Epoch-relative time that rule takes effect.
*/
static time_t
transtime(janfirst, year, rulep, offset)
time_t				janfirst;
int				year;
register const struct rule *	rulep;
long				offset;
{
	register int	leapyear;
	register time_t	value;
	register int	i;
	int		d, m1, yy0, yy1, yy2, dow;

	leapyear = isleap(year);
	switch (rulep->r_type) {

	case JULIAN_DAY:
		/*
		** Jn - Julian day, 1 == January 1, 60 == March 1 even in leap
		** years.
		** In non-leap years, or if the day number is 59 or less, just
		** add SECSPERDAY times the day number-1 to the time of
		** January 1, midnight, to get the day.
		*/
		value = janfirst + (rulep->r_day - 1) * SECSPERDAY;
		if (leapyear && rulep->r_day >= 60)
			value += SECSPERDAY;
		break;

	case DAY_OF_YEAR:
		/*
		** n - day of year.
		** Just add SECSPERDAY times the day number to the time of
		** January 1, midnight, to get the day.
		*/
		value = janfirst + rulep->r_day * SECSPERDAY;
		break;

	case MONTH_NTH_DAY_OF_WEEK:
		/*
		** Mm.n.d - nth "dth day" of month m.
		*/
		value = janfirst;
		for (i = 0; i < rulep->r_mon - 1; ++i)
			value += mon_lengths[leapyear][i] * SECSPERDAY;

		/*
		** Use Zeller's Congruence to get day-of-week of first day of
		** month.
		*/
		m1 = (rulep->r_mon + 9) % 12 + 1;
		yy0 = (rulep->r_mon <= 2) ? (year - 1) : year;
		yy1 = yy0 / 100;
		yy2 = yy0 % 100;
		dow = ((26 * m1 - 2) / 10 +
			1 + yy2 + yy2 / 4 + yy1 / 4 - 2 * yy1) % 7;
		if (dow < 0)
			dow += DAYSPERWEEK;

		/*
		** "dow" is the day-of-week of the first day of the month.  Get
		** the day-of-month (zero-origin) of the first "dow" day of the
		** month.
		*/
		d = rulep->r_day - dow;
		if (d < 0)
			d += DAYSPERWEEK;
		for (i = 1; i < rulep->r_week; ++i) {
			if (d + DAYSPERWEEK >=
				mon_lengths[leapyear][rulep->r_mon - 1])
					break;
			d += DAYSPERWEEK;
		}

		/*
		** "d" is the day-of-month (zero-origin) of the day we want.
		*/
		value += d * SECSPERDAY;
		break;

#ifdef BROKEN
	case MONTH_DAY:
		/*
		** Mm.n.d - dth day of week n of month m.
		*/
		value = janfirst;
		for (i = 0; i < rulep->r_mon - 1; ++i)
			value += mon_lengths[leapyear][i] * SECSPERDAY;

		if (rulep->r_week == 5) {
			/*
			** Get day number of last day of month.
			*/
			d = mon_lengths[leapyear][rulep->r_mon - 1];
		} else {
			/*
			** Get day number of first day of month.
			*/
			d = 1;
		}

		/*
		** Use Zeller's Congruence to get day-of-week of that
		** day.
		*/
		m1 = (rulep->r_mon + 9) % 12 + 1;
		yy0 = (rulep->r_mon <= 2) ? (year - 1) : year;
		yy1 = yy0 / 100;
		yy2 = yy0 % 100;
		dow = ((26 * m1 - 2) / 10 +
			d + yy2 + yy2 / 4 + yy1 / 4 - 2 * yy1) % 7;
		if (dow < 0)
			dow += DAYSPERWEEK;

		if (rulep->r_week == 5) {
			/*
			** "d" is the day-of-month of the last "dow" day of
			** the month.  Step backwards through the month
			** (decrementing "d", and decrementing "dow" modulo 7)
			** until we have the day-of-week that we want.
			*/
			while (dow != rulep->r_day) {
				--d;
				--dow;
				if (dow < 0)
					dow += DAYSPERWEEK;
			}
		} else {
			/*
			** "d" is the day-of-month of the first "dow" day of
			** the month.  Get the "day-of-month" (probably
			** negative, meaning Sunday of that week was last
			** month) of the Sunday of the first week of the month.
			*/
			d -= dow;

			/*
			** Now get the "day-of-month" of the Sunday beginning
			** the week we're interested in.
			*/
			d += (rulep->r_week - 1) * DAYSPERWEEK;

			/*
			** Now get the "day-of-month" of the day of week we're
			** interested in.
			*/
			d += rulep->r_day;
		}

		/*
		** "d" is the day-of-month of the day we want.
		*/
		value += (d - 1) * SECSPERDAY;
		break;
#endif
	}

	/*
	** "value" is the Epoch-relative time of 00:00:00 GMT on the day in
	** question.  To get the Epoch-relative time of the specified local
	** time on that day, add the transition time and the current offset
	** from GMT.
	*/
	return value + rulep->r_time + offset;
}

/*
** Given a POSIX section 8-style TZ string, fill in the rule tables as
** appropriate.
*/
static int
tzparse(name, sp)
const char *		name;
register struct state *	sp;
{
	char *				stdname;
	char *				dstname;
	int				stdlen;
	int				dstlen;
	long				stdoffset;
	long				dstoffset;
	struct rule			start;
	struct rule			end;
	register int			year;
	register time_t			janfirst;
	register time_t *		atp;
	register unsigned char *	typep;
	register char *			cp;
	register int			i;
	time_t				starttime;
	time_t				endtime;

	stdname = name;
	name = getzname(name);
	stdlen = name - stdname;	/* length of standard zone name */
	if (stdlen == 0)
		return -1;
	name = getoffset(name, &stdoffset);
	if (name == NULL)
		return -1;
	if (*name != '\0') {
		dstname = name;
		name = getzname(name);
		dstlen = name - dstname;	/* length of DST zone name */
		if (dstlen == 0)
			return -1;
		if (*name != '\0' && *name != ',' && *name != ';') {
			name = getoffset(name, &dstoffset);
			if (name == NULL)
				return -1;
		} else
			dstoffset = stdoffset - 1 * SECSPERHOUR;
		if (stdlen + 1 + dstlen + 1 > sizeof sp->chars)
			return -1;
		if (*name == ',' || *name == ';') {
			++name;
			if ((name = getrule(name, &start)) == NULL)
				return -1;
			if (*name++ != ',')
				return -1;
			if ((name = getrule(name, &end)) == NULL)
				return -1;
			if (*name != '\0')
				return -1;
			sp->typecnt = 2;	/* standard time and DST */
			/*
			** Two transitions per year, from 1970 to 2038.
			*/
			sp->timecnt = 2 * (2038 - 1970 + 1);
			if (sp->timecnt > TZ_MAX_TIMES)
				return -1;
			sp->ttis[0].tt_gmtoff = -dstoffset;
			sp->ttis[0].tt_isdst = 1;
			sp->ttis[0].tt_abbrind = stdlen + 1;
			sp->ttis[1].tt_gmtoff = -stdoffset;
			sp->ttis[1].tt_isdst = 0;
			sp->ttis[1].tt_abbrind = 0;
			atp = sp->ats;
			typep = sp->types;
			for (year = 1970, janfirst = 0; year <= 2038; year++) {
				starttime = transtime(janfirst, year, &start,
					stdoffset);
				endtime = transtime(janfirst, year, &end,
					dstoffset);
				if (starttime > endtime) {
					*atp++ = endtime;
					*typep++ = 1;	/* DST ends */
					*atp++ = starttime;
					*typep++ = 0;	/* DST begins */
				} else {
					*atp++ = starttime;
					*typep++ = 0;	/* DST begins */
					*atp++ = endtime;
					*typep++ = 1;	/* DST ends */
				}
				janfirst +=
					year_lengths[isleap(year)] * SECSPERDAY;
			}
		} else {
			register int	i;

			if (*name != '\0')
				return -1;
			if (tzload("_ST0_DT", sp) != 0)
				return -1;
			/*
			** Adjust the types.
			*/
			for (i = 0; i < sp->typecnt; ++i)
				if (sp->ttis[i].tt_isdst) {
					sp->ttis[i].tt_gmtoff = -dstoffset;
					sp->ttis[i].tt_abbrind = stdlen + 1;
				} else {
					sp->ttis[i].tt_gmtoff = -stdoffset;
					sp->ttis[i].tt_abbrind = 0;
				}
			/*
			** Adjust the times.
			*/
			for (i = 0; i < sp->timecnt; ++i)
				sp->ats[i] += stdoffset;	/* -= stdoff? */
		}
	} else {
		dstlen = 0;
		sp->typecnt = 1;		/* only standard time */
		sp->timecnt = 0;
		sp->ttis[0].tt_gmtoff = -stdoffset;
		sp->ttis[0].tt_isdst = 0;
		sp->ttis[0].tt_abbrind = 0;
	}
	sp->charcnt = stdlen + 1 + dstlen + 1;
  	if (sp->charcnt > sizeof sp->chars)
  		return -1;
	cp = sp->chars;
	(void) strncpy(cp, stdname, stdlen);
	cp += stdlen;
	*cp++ = '\0';
	if (dstlen > 0) {
		(void) strncpy(cp, dstname, dstlen);
		*(cp + dstlen) = '\0';
	}
	sp->leapcnt = 0;		/* so, we're off a little */
	if (sp == &lclstate)
		settzname(sp);
	return 0;
}

static void
tzsetgmt(sp)
register struct state *	sp;
{
	sp->leapcnt = 0;		/* so, we're off a little */
	sp->timecnt = 0;
	sp->ttis[0].tt_gmtoff = 0;
	sp->ttis[0].tt_abbrind = 0;
	(void) strcpy(sp->chars, "GMT");
	if (sp == &lclstate)
		settzname(sp);
}

void
tzset()
{
	register const char *	name;

	lcl_is_set = TRUE;
	name = getenv("TZ");
	if (name != 0 && *name == '\0')
		tzsetgmt(&lclstate);		/* GMT by request */
	else if (tzload(name, &lclstate) != 0) {
		if (name[0] == ':' || tzparse(name, &lclstate) != 0)
			tzsetgmt(&lclstate);
	}
}

void
tzsetwall()
{
	lcl_is_set = TRUE;
	if (tzload((char *) 0, &lclstate) != 0)
		tzsetgmt(&lclstate);
}

struct tm *
localtime(timep)
const time_t *	timep;
{
	register const struct state *	sp;
	register const struct ttinfo *	ttisp;
	register int			i;
	time_t				t;
	static struct tm		tm;

	if (!lcl_is_set)
		tzset();
	sp = &lclstate;
	t = *timep;
	if (sp->timecnt == 0 || t < sp->ats[0]) {
		i = 0;
		while (sp->ttis[i].tt_isdst)
			if (++i >= sp->typecnt) {
				i = 0;
				break;
			}
	} else {
		for (i = 1; i < sp->timecnt; ++i)
			if (t < sp->ats[i])
				break;
		i = sp->types[i - 1];
	}
	ttisp = &sp->ttis[i];
	/*
	** To get (wrong) behavior that's compatible with System V Release 2.0
	** you'd replace the statement below with
	**	t += ttisp->tt_gmtoff;
	**	timesub(&t, 0L, sp, &tm);
	*/
	timesub(&t, ttisp->tt_gmtoff, sp, &tm);
	tm.tm_isdst = ttisp->tt_isdst;
	tzname[tm.tm_isdst] = &sp->chars[ttisp->tt_abbrind];
#ifdef KRE_COMPAT
	tm.tm_zone = &sp->chars[ttisp->tt_abbrind];
#endif /* defined KRE_COMPAT */
#ifdef TZA_COMPAT
	tz_abbr = &sp->chars[ttisp->tt_abbrind];
#endif /* defined TZA_COMPAT */
	return &tm;
}

struct tm *
gmtime(clock)
const time_t *	clock;
{
	static struct tm	tm;

	if (!gmt_is_set) {
		gmt_is_set = TRUE;
		if (tzload("GMT", &gmtstate) != 0)
			tzsetgmt(&gmtstate);
	}
	timesub(clock, 0L, &gmtstate, &tm);
#ifdef KRE_COMPAT
	tm.tm_zone = "GMT";		/* UCT ? */
#endif /* defined KRE_COMPAT */
	return &tm;
}

#ifdef STD_INSPIRED

struct tm *
offtime(clock, offset)
const time_t *	clock;
long		offset;
{
	static struct tm	tm;

	if (!gmt_is_set) {
		gmt_is_set = TRUE;
		if (tzload("GMT", &gmtstate) != 0)
			tzsetgmt(&gmtstate);
	}
	timesub(clock, offset, &gmtstate, &tm);
	return &tm;
}

#endif /* defined STD_INSPIRED */

static void
timesub(clock, offset, sp, tmp)
const time_t *			clock;
long				offset;
register const struct state *	sp;
register struct tm *		tmp;
{
	register const struct lsinfo *	lp;
	register long			days;
	register long			rem;
	register int			y;
	register int			yleap;
	register const int *		ip;
	register long			corr;
	register int			hit;

	corr = 0;
	hit = FALSE;
	y = sp->leapcnt;
	while (--y >= 0) {
		lp = &sp->lsis[y];
		if (*clock >= lp->ls_trans) {
			if (*clock == lp->ls_trans)
				hit = ((y == 0 && lp->ls_corr > 0) ||
					lp->ls_corr > sp->lsis[y-1].ls_corr);
			corr = lp->ls_corr;
			break;
		}
	}
	days = *clock / SECSPERDAY;
	rem = *clock % SECSPERDAY;
#ifdef mc68k
	if (*clock == 0x80000000) {
		/*
		** A 3B1 muffs the division on the most negative number.
		*/
		days = -24855;
		rem = -11648;
	}
#endif /* mc68k */
	rem += (offset - corr);
	while (rem < 0) {
		rem += SECSPERDAY;
		--days;
	}
	while (rem >= SECSPERDAY) {
		rem -= SECSPERDAY;
		++days;
	}
	tmp->tm_hour = (int) (rem / SECSPERHOUR);
	rem = rem % SECSPERHOUR;
	tmp->tm_min = (int) (rem / SECSPERMIN);
	tmp->tm_sec = (int) (rem % SECSPERMIN);
	if (hit)
		/*
		 * A positive leap second requires a special
		 * representation.  This uses "... ??:59:60".
		 */
		 tmp->tm_sec += 1;
	tmp->tm_wday = (int) ((EPOCH_WDAY + days) % DAYSPERWEEK);
	if (tmp->tm_wday < 0)
		tmp->tm_wday += DAYSPERWEEK;
	y = EPOCH_YEAR;
	if (days >= 0)
		for ( ; ; ) {
			yleap = isleap(y);
			if (days < (long) year_lengths[yleap])
				break;
			++y;
			days = days - (long) year_lengths[yleap];
		}
	else do {
		--y;
		yleap = isleap(y);
		days = days + (long) year_lengths[yleap];
	} while (days < 0);
	tmp->tm_year = y - TM_YEAR_BASE;
	tmp->tm_yday = (int) days;
	ip = mon_lengths[yleap];
	for (tmp->tm_mon = 0; days >= (long) ip[tmp->tm_mon]; ++(tmp->tm_mon))
		days = days - (long) ip[tmp->tm_mon];
	tmp->tm_mday = (int) (days + 1);
	tmp->tm_isdst = 0;
#ifdef KRE_COMPAT
	tmp->tm_zone = "";
	tmp->tm_gmtoff = offset;
#endif /* defined KRE_COMPAT */
}

#ifdef BSD_COMPAT

/*
** If ctime and localtime aren't in the same file on 4.3BSD systems,
** you can run into compilation problems--take
**	cc date.c -lz
** (please).
*/

char *
ctime(timep)
const time_t *	timep;
{
	return asctime(localtime(timep));
}

#endif /* defined BSD_COMPAT */
