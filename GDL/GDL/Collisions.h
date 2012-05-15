/** \file
 *  Game Develop
 *  2008-2012 Florian Rival (Florian.Rival@gmail.com)
 */

#ifndef COLLISIONS_H_INCLUDED
#define COLLISIONS_H_INCLUDED

#include <iostream>
#include <vector>
#include <string>
#include "GDL/SpriteObject.h"
#include <cmath>

/**
 * \brief Pixel perfect collision test between two sprite objects
 * \ingroup SpriteObjectExtension
 * Alpha transparency, rotation and zooms are taken into account.
 *
 * \return true if the sprite are overlapping
 */
bool GD_API CheckCollision( const SpriteObject* const objet1, const SpriteObject* const objet2);

#endif // COLLISIONS_H_INCLUDED
