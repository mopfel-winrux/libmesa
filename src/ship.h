/// @file
///
/// Ship identifiers, extracted from vere/pkg/noun/ship.h with the noun
/// conversions replaced by pure-C @p rendering and parsing.

#ifndef U3_SHIP_H
#define U3_SHIP_H

#include "c3/c3.h"

typedef c3_d u3_ship[2];

void
u3_ship_to_bytes(u3_ship who_u, c3_y len_y, c3_y* buf_y);

void
u3_ship_of_bytes(u3_ship who_u, c3_y len_y, c3_y* buf_y);

/* u3_ship_to_string(): render as @p (e.g. "~sampel-palnet"); caller frees.
*/
c3_c*
u3_ship_to_string(u3_ship who_u);

/* u3_ship_of_string(): parse @p text (leading '~' optional); c3n on failure.
*/
c3_o
u3_ship_of_string(u3_ship who_u, const c3_c* str_c);

c3_o
u3_ships_equal(u3_ship sip_u, u3_ship sap_u);

c3_h
u3_ship_rank(u3_ship who_u);

void
u3_ship_copy(u3_ship des_u, u3_ship src_u);

/* u3_ship_sein(): default sponsor (+sein:title without jael); comets
**                 and planets map to their star, stars and moons to
**                 their parent, galaxies to themselves.
*/
void
u3_ship_sein(u3_ship par_u, u3_ship who_u);

/**
* Returns a ship's galaxy byte prefix.
*/
c3_y
u3_ship_czar(u3_ship who_u);

/**
* Returns a ship's star prefix.
*/
c3_s
u3_ship_king(u3_ship who_u);

/**
* Returns a ship's planet prefix.
*/
c3_h
u3_ship_duke(u3_ship who_u);

/**
* Returns a ship's moon prefix.
*/
c3_d
u3_ship_earl(u3_ship who_u);

/* u3_ob_fein(), u3_ob_fynd(): +fein:ob / +fynd:ob on 64-bit ships.
*/
c3_d
u3_ob_fein(c3_d pyn_d);

c3_d
u3_ob_fynd(c3_d pyn_d);

#endif /* ifndef U3_SHIP_H */
