#pragma once

// Copy this file to ical_secrets.h (gitignored) and fill it in. Empty leaves
// that part of the calendar out.
//
// Where the timetable's four feeds are, each at HOST/<name>: Lectures,
// Practicals, Exams and Other. No trailing slash.
#define ICAL_TIMETABLE_HOST ""

// A calendar's published .ics link, as Outlook gives one, which carries its own
// access token. Only its events whose title starts with "werk" are kept.
#define ICAL_WORK_URL ""
