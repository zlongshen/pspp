/* PSPPIRE - a graphical user interface for PSPP.
   Copyright (C) 2007, 2009  Free Software Foundation

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

#include <config.h>

#include "executor.h"
#include "psppire-data-store.h"
#include <data/lazy-casereader.h>
#include <data/procedure.h>
#include <libpspp/getl.h>
#include <language/lexer/lexer.h>
#include <language/command.h>
#include <output/manager.h>
#include "psppire-output-window.h"

extern struct dataset *the_dataset;
extern struct source_stream *the_source_stream;
extern PsppireDataStore *the_data_store;

/* Lazy casereader callback function used by execute_syntax. */
static struct casereader *
create_casereader_from_data_store (void *data_store_)
{
  PsppireDataStore *data_store = data_store_;
  return psppire_data_store_get_reader (data_store);
}

gboolean
execute_syntax (struct getl_interface *sss)
{
  struct lexer *lexer;
  gboolean retval = TRUE;

  struct casereader *reader;
  const struct caseproto *proto;
  casenumber case_cnt;
  unsigned long int lazy_serial;

  /* When the user executes a number of snippets of syntax in a
     row, none of which read from the active file, the GUI becomes
     progressively less responsive.  The reason is that each syntax
     execution encapsulates the active file data in another
     datasheet layer.  The cumulative effect of having a number of
     layers of datasheets wastes time and space.

     To solve the problem, we use a "lazy casereader", a wrapper
     around the casereader obtained from the data store, that
     only actually instantiates that casereader when it is
     needed.  If the data store casereader is never needed, then
     it is reused the next time syntax is run, without wrapping
     it in another layer. */
  proto = psppire_data_store_get_proto (the_data_store);
  case_cnt = psppire_data_store_get_case_count (the_data_store);
  reader = lazy_casereader_create (proto, case_cnt,
                                   create_casereader_from_data_store,
                                   the_data_store, &lazy_serial);
  proc_set_active_file_data (the_dataset, reader);

  g_return_val_if_fail (proc_has_active_file (the_dataset), FALSE);

  lexer = lex_create (the_source_stream);

  getl_append_source (the_source_stream, sss, GETL_BATCH, ERRMODE_CONTINUE);

  for (;;)
    {
      enum cmd_result result = cmd_parse (lexer, the_dataset);

      if ( cmd_result_is_failure (result))
	{
	  retval = FALSE;
	  if ( source_stream_current_error_mode (the_source_stream)
	       == ERRMODE_STOP )
	    break;
	}

      if ( result == CMD_EOF || result == CMD_FINISH)
	break;
    }

  getl_abort_noninteractive (the_source_stream);

  lex_destroy (lexer);

  psppire_dict_replace_dictionary (the_data_store->dict,
				   dataset_dict (the_dataset));

  reader = proc_extract_active_file_data (the_dataset);
  if (!lazy_casereader_destroy (reader, lazy_serial))
    psppire_data_store_set_reader (the_data_store, reader);

  som_flush ();

  return retval;
}