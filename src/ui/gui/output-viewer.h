/* PSPPIRE - a graphical user interface for PSPP.
   Copyright (C) 2007  Free Software Foundation

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>. */


#ifndef OUTPUT_VIEWER_H
#define OUTPUT_VIEWER_H

#include <gtk/gtk.h>

#include "window-manager.h"


extern int viewer_length ;
extern int viewer_width ;

struct output_viewer * new_output_viewer (void);

void reload_viewer (struct output_viewer *);

void reload_the_viewer (void);


const char * output_file_name (void);

#endif