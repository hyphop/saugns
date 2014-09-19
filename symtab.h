/* Copyright (c) 2011-2012 Joel K. Pettersson <joelkpettersson@gmail.com>
 *
 * This file and the software of which it is part is distributed under the
 * terms of the GNU Lesser General Public License, either version 3 or (at
 * your option) any later version; WITHOUT ANY WARRANTY, not even of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * View the file COPYING for details, or if missing, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef __SGS_symtab_h
#define __SGS_symtab_h

struct SGSSymtab;
typedef struct SGSSymtab SGSSymtab;

SGSSymtab* SGS_create_symtab(void);
void SGS_destroy_symtab(SGSSymtab *o);

int SGS_symtab_register_str(SGSSymtab *o, const char *str);
const char *SGS_symtab_lookup_str(SGSSymtab *o, int id);

void* SGS_symtab_get(SGSSymtab *o, const char *key);
void* SGS_symtab_set(SGSSymtab *o, const char *key, void *value);

#endif /* EOF */
