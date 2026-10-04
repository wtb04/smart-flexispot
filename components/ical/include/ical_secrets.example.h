#pragma once

// Copy this file to ical_secrets.h (gitignored) and fill it in. Empty leaves
// that part of the calendar out.
//
// Where the timetable's four feeds are, each at HOST/<name>: Lectures,
// Practicals, Exams and Other. No trailing slash.
#define ICAL_TIMETABLE_HOST ""

// A calendar's published .ics link, as Outlook gives one, which carries its own
// access token.
#define ICAL_WORK_URL ""

// Only that calendar's events whose title starts with this, in lower case, as
// "work" for "Work" and "Workshop"; empty keeps them all.
#define ICAL_WORK_KEEP ""
