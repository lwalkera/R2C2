/* Copyright (C) 2009-2010 Michael Moon aka Triffid_Hunter   */
/* Copyright (c) 2011 Jorge Pinto - casainho@gmail.com       */
/* All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:

   * Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
   * Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in
     the documentation and/or other materials provided with the
     distribution.
   * Neither the name of the copyright holders nor the names of
     contributors may be used to endorse or promote products derived
     from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.
*/

#include	"gcode_parse.h"

#include	<string.h>
#include <stdbool.h>

#include	"serial.h"
#include	"sermsg.h"
#include	"dda_queue.h"

#include	"gcode_process.h"
#include        "machine.h"
#include        "config.h"

/*
   Switch user friendly values to coding friendly values

   This also affects the possible build volume. We have +-2^31 numbers available and as we internally measure position in steps and use a precision factor of 1000, this translates into a possible range of

   2^31 mm / STEPS_PER_MM_x / 1000

   for each axis. For a M6 threaded rod driven machine and 1/16 microstepping this evaluates to

   2^31 mm / 200 / 1 / 16 / 1000 = 671 mm,

   which is about the worst case we have. All other machines have a bigger build volume.
   */

uint32_t steps_per_m_x;
uint32_t steps_per_m_y;
uint32_t steps_per_m_z;
uint32_t steps_per_m_e;

double steps_per_in_x;
double steps_per_in_y;
double steps_per_in_z;
double steps_per_in_e;


void gcode_parse_init(void)
{
	steps_per_m_x = ((uint32_t) (config.steps_per_mm_x * 1000.0));
	steps_per_m_y = ((uint32_t) (config.steps_per_mm_y * 1000.0));
	steps_per_m_z = ((uint32_t) (config.steps_per_mm_z * 1000.0));
	steps_per_m_e = ((uint32_t) (config.steps_per_mm_e * 1000.0));

	// same as above with 25.4 scale factor
	steps_per_in_x = ((double) (25.4 * config.steps_per_mm_x));
	steps_per_in_y = ((double) (25.4 * config.steps_per_mm_y));
	steps_per_in_z = ((double) (25.4 * config.steps_per_mm_z));
	steps_per_in_e = ((double) (25.4 * config.steps_per_mm_e));
}

uint8_t last_field = 0;

#define crc(a, b)		(a ^ b)

decfloat read_digit;

GCODE_COMMAND next_target;

/*
   utility functions
   */

int32_t	decfloat_to_int(decfloat *df, int32_t multiplicand, int32_t denominator) {
	int64_t	r = df->mantissa;
	uint8_t	e = df->exponent;

	// e=1 means we've seen a decimal point but no digits after it, and e=2 means we've seen a decimal point with one digit so it's too high by one if not zero
	if (e)
		e--;

	// scale factors
	if (multiplicand != 1)
		r *= multiplicand;
	if (denominator != 1)
		r /= denominator;

	// sign
	if (df->sign)
		r = -r;

	// exponent- try to keep divides to a minimum for common (small) values at expense of slightly more code
	while (e >= 5) {
		r /= 100000;
		e -= 5;
	}

	if (e == 1)
		r /= 10;
	else if (e == 2)
		r /= 100;
	else if (e == 3)
		r /= 1000;
	else if (e == 4)
		r /= 10000;

	return r;
}


/*
   public functions
   */

void SpecialMoveXY(int32_t x, int32_t y, uint32_t f) {
	TARGET t = startpoint;
	t.X = x;
	t.Y = y;
	t.F = f;
	t.options.g28 = 1; /* signal a G28 command */
	enqueue(&t);
}

void SpecialMoveZ(int32_t z, uint32_t f) {
	TARGET t = startpoint;
	t.Z = z;
	t.F = f;
	t.options.g28 = 1; /* signal a G28 command */
	enqueue(&t);
}

void SpecialMoveE(int32_t e, uint32_t f) {
	TARGET t = startpoint;
	t.E = e;
	t.F = f;
	t.options.g28 = 1; /* signal a G28 command */
	enqueue(&t);
}

void gcode_parse_line (tLineBuffer *pLine) 
{
	unsigned int j;

	for (j=0; j < pLine->len; j++)
		gcode_parse_char (pLine->data [j], pLine);
}

/****************************************************************************
 *                                                                           *
 * Character Received - add it to our command                                *
 *                                                                           *
 ****************************************************************************/

void gcode_parse_char(uint8_t c, tLineBuffer *pLine) {
	bool send_reply = true;

#ifdef ASTERISK_IN_CHECKSUM_INCLUDED
	if (next_target.seen_checksum == 0)
		next_target.checksum_calculated = crc(next_target.checksum_calculated, c);
#endif

	// uppercase
	if (c >= 'a' && c <= 'z')
		c &= ~32;

	// process previous field
	if (last_field) {
		// check if we're seeing a new field or end of line
		// any character will start a new field, even invalid/unknown ones
		if ((c >= 'A' && c <= 'Z') || c == '*' || (c == 10) || (c == 13)) {
			switch (last_field) {
				case 'G':
					next_target.G = read_digit.mantissa;
					break;
				case 'M':
					next_target.M = read_digit.mantissa;
					// this is a bit hacky since string parameters don't fit in general G code syntax
					// NB: filename MUST start with a letter and MUST NOT contain spaces
					// letters will also be converted to uppercase
					if ( (next_target.M == 23) || (next_target.M == 28) )
					{
						next_target.getting_string = 1;
					}
					break;
				case 'X':
					if (next_target.option_inches)
						next_target.target.X = decfloat_to_int(&read_digit, steps_per_in_x, 1);
					else
						next_target.target.X = decfloat_to_int(&read_digit, steps_per_m_x, 1000);
					break;
				case 'Y':
					if (next_target.option_inches)
						next_target.target.Y = decfloat_to_int(&read_digit, steps_per_in_y, 1);
					else
						next_target.target.Y = decfloat_to_int(&read_digit, steps_per_m_y, 1000);
					break;
				case 'Z':
					if (next_target.option_inches)
						next_target.target.Z = decfloat_to_int(&read_digit, steps_per_in_z, 1);
					else
						next_target.target.Z = decfloat_to_int(&read_digit, steps_per_m_z, 1000);
					break;
				case 'E':
					if (next_target.option_inches)
						next_target.target.E = decfloat_to_int(&read_digit, steps_per_in_e, 1);
					else
						next_target.target.E = decfloat_to_int(&read_digit, steps_per_m_e, 1000);
					break;
				case 'F':
					// just use raw integer, we need move distance and n_steps to convert it to a useful value, so wait until we have those to convert it
					if (next_target.option_inches)
						next_target.target.F = decfloat_to_int(&read_digit, 254, 10);
					else
						next_target.target.F = decfloat_to_int(&read_digit, 1, 1);
					break;
				case 'S':
					next_target.S = decfloat_to_int(&read_digit, 1, 1);
					break;
				case 'P':
					// if this is dwell, multiply by 1000 to convert seconds to milliseconds
					if (next_target.G == 4)
						next_target.P = decfloat_to_int(&read_digit, 1000, 1);
					else
						next_target.P = decfloat_to_int(&read_digit, 1, 1);
					break;
				case 'N':
					next_target.N = decfloat_to_int(&read_digit, 1, 1);
					break;
				case '*':
					next_target.checksum_read = decfloat_to_int(&read_digit, 1, 1);
					break;
			}
			// reset for next field
			last_field = 0;
			read_digit.sign = read_digit.mantissa = read_digit.exponent = 0;
		}
	}

	if (next_target.getting_string)
	{
		if ((c == 10) || (c == 13) || ( c == ' ')  || ( c == '*'))
			next_target.getting_string = 0;
		else
		{
			if (next_target.chpos < sizeof(next_target.filename))
			{
				next_target.filename [next_target.chpos++] = c;
				next_target.filename [next_target.chpos] = 0;
			}
		}      
	}

	// skip comments, filenames
	if (next_target.seen_semi_comment == 0 && next_target.seen_parens_comment == 0 && next_target.getting_string == 0) {
		// new field?
		if ((c >= 'A' && c <= 'Z') || c == '*') {
			last_field = c;
		}

		// process character
		switch (c) {
			// each currently known command is either G or M, so preserve previous G/M unless a new one has appeared
			// FIXME: same for T command
			case 'G':
				next_target.seen_G = 1;
				next_target.seen_M = 0;
				next_target.M = 0;
				break;
			case 'M':
				next_target.seen_M = 1;
				next_target.seen_G = 0;
				next_target.G = 0;
				break;
			case 'X':
				next_target.seen_X = 1;
				break;
			case 'Y':
				next_target.seen_Y = 1;
				break;
			case 'Z':
				next_target.seen_Z = 1;
				break;
			case 'E':
				next_target.seen_E = 1;
				break;
			case 'F':
				next_target.seen_F = 1;
				break;
			case 'S':
				next_target.seen_S = 1;
				break;
			case 'P':
				next_target.seen_P = 1;
				break;
			case 'N':
				next_target.seen_N = 1;
				break;
			case '*':
				next_target.seen_checksum = 1;
				break;

				// comments
			case ';':
				next_target.seen_semi_comment = 1;
				break;
			case '(':
				next_target.seen_parens_comment = 1;
				break;

				// now for some numeracy
			case '-':
				read_digit.sign = 1;
				// force sign to be at start of number, so 1-2 = -2 instead of -12
				read_digit.exponent = 0;
				read_digit.mantissa = 0;
				break;
			case '.':
				if (read_digit.exponent == 0)
					read_digit.exponent = 1;
				break;


			default:
				// can't do ranges in switch..case, so process actual digits here
				if (c >= '0' && c <= '9') {
					// this is simply mantissa = (mantissa * 10) + atoi(c) in different clothes
					read_digit.mantissa = (read_digit.mantissa << 3) + (read_digit.mantissa << 1) + (c - '0');
					if (read_digit.exponent)
						read_digit.exponent++;
				}
		}
	} else if ( next_target.seen_parens_comment == 1 && c == ')')
		next_target.seen_parens_comment = 0; // recognize stuff after a (comment)


#ifndef ASTERISK_IN_CHECKSUM_INCLUDED
	if (next_target.seen_checksum == 0)
		next_target.checksum_calculated = crc(next_target.checksum_calculated, c);
#endif

	// end of line
	if ((c == 10) || (c == 13)) {
		if (
#ifdef	REQUIRE_LINENUMBER
				(next_target.N >= next_target.N_expected) && (next_target.seen_N == 1)
#else
				1
#endif
		   ) {
			if (
#ifdef	REQUIRE_CHECKSUM
					((next_target.checksum_calculated == next_target.checksum_read) && (next_target.seen_checksum == 1))
#else
					((next_target.checksum_calculated == next_target.checksum_read) || (next_target.seen_checksum == 0))
#endif
			   ) {
				if (sd_writing_file)
				{
					if (next_target.seen_M && (next_target.M >= 20) && (next_target.M <= 29) )
					{
						if (next_target.seen_M && next_target.M == 29)
						{ 
							// M29 - stop writing
							sd_writing_file = false;
							sd_close (&file);
							serial_writestr("Done saving file\r\n");
						}
						else
						{
							// else - do not write SD M-codes to file
							serial_writestr("ok\r\n");
						}
					}
					else
					{
						// lines in files must be LF terminated for sd_read_file to work
						if (pLine->data [pLine->len-1] == 13)
							pLine->data [pLine->len-1] = 10;

						if (sd_write_to_file(pLine->data, pLine->len))
							serial_writestr("ok\r\n");
						else
							serial_writestr("error writing to file\r\n");
					}
				}
				else
				{
					// process
					send_reply = process_gcode_command();

					/* do not reply the "ok" for some commands which generate own reply */
					if (send_reply)
					{
						serial_writestr("ok\r\n");
					}

					// expect next line number
					if (next_target.seen_N == 1)
						next_target.N_expected = next_target.N + 1;
				}
			}
			else {
				serial_writestr("Expected checksum ");
				serwrite_uint8(next_target.checksum_calculated);
				serial_writestr("\r\n");
				request_resend();
			}
		}
		else {
			serial_writestr("Expected line number ");
			serwrite_uint32(next_target.N_expected);
			serial_writestr("\r\n");
			request_resend();
		}

		// reset variables
		next_target.seen_X = next_target.seen_Y = next_target.seen_Z = \
							 next_target.seen_E = next_target.seen_F = next_target.seen_S = \
							 next_target.seen_P = next_target.seen_N = next_target.seen_M = \
							 next_target.seen_checksum = next_target.seen_semi_comment = \
							 next_target.seen_parens_comment = next_target.checksum_read = \
							 next_target.checksum_calculated = 0;
		next_target.chpos = 0;
		last_field = 0;
		read_digit.sign = read_digit.mantissa = read_digit.exponent = 0;

		// assume a G1 by default
		next_target.seen_G = 1;
		next_target.G = 1;

		if (next_target.option_relative) {
			next_target.target.X = next_target.target.Y = next_target.target.Z = 0;
			next_target.target.E = 0;
		}
	}
}

/****************************************************************************
 *                                                                           *
 * Request a resend of the current line - used from various places.          *
 *                                                                           *
 * Relies on the global variable next_target.N being valid.                  *
 *                                                                           *
 ****************************************************************************/

void request_resend(void) {
	serial_writestr("rs ");
	serwrite_uint8(next_target.N);
	serial_writestr("\r\n");
}
