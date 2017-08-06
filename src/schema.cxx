﻿/*
 * Copyright 2016-2017 libfpta authors: please see AUTHORS file.
 *
 * This file is part of libfpta, aka "Fast Positive Tables".
 *
 * libfpta is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libfpta is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libfpta.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "details.h"

static __inline fpta_shove_t fpta_dbi_shove(const fpta_shove_t table_shove,
                                            const size_t index_id) {
  assert(table_shove > fpta_flag_table);
  assert(index_id < fpta_max_indexes);

  fpta_shove_t dbi_shove = table_shove - fpta_flag_table;
  assert(0 == (dbi_shove & (fpta_column_typeid_mask | fpta_column_index_mask)));
  dbi_shove += index_id;

  assert(fpta_shove_eq(table_shove, dbi_shove));
  return dbi_shove;
}

static __hot fpta_shove_t fpta_shove_name(const char *name,
                                          enum fpta_schema_item type) {
  char uppercase[fpta_name_len_max];
  size_t i, len = strlen(name);

  for (i = 0; i < len && i < sizeof(uppercase); ++i)
    uppercase[i] = (char)toupper(name[i]);

  fpta_shove_t shove = t1ha(uppercase, i, type) << fpta_name_hash_shift;
  if (type == fpta_table)
    shove |= fpta_flag_table;
  return shove;
}

template <bool first> static __inline bool is_valid_char4name(char c) {
  if (first ? isalpha(c) : isalnum(c))
    return true;
  if (c == '_')
    return true;
  if (FPTA_ALLOW_DOT4NAMES && c == '.')
    return true;

  return false;
}

bool fpta_validate_name(const char *name) {
  if (unlikely(name == nullptr))
    return false;

  if (unlikely(!is_valid_char4name<true>(name[0])))
    return false;

  size_t i = 1;
  while (name[i]) {
    if (unlikely(!is_valid_char4name<false>(name[i])))
      return false;
    if (unlikely(++i > fpta_name_len_max))
      return false;
  }

  if (unlikely(i < fpta_name_len_min))
    return false;

  return fpta_shove_name(name, fpta_column) > (1 << fpta_name_hash_shift);
}

struct fpta_dbi_name {
  char cstr[(64 + 6 - 1) / 6 /* 64-битный хэш */ + 1 /* терминирующий 0 */];
};

static void fpta_shove2str(fpta_shove_t shove, fpta_dbi_name *name) {
  const static char aplhabet[65] =
      "@0123456789qwertyuiopasdfghjklzxcvbnmQWERTYUIOPASDFGHJKLZXCVBNM_";

  char *buf = name->cstr;
  do
    *buf++ = aplhabet[shove & 63];
  while (shove >>= 6);

  *buf = '\0';
  assert(buf < name->cstr + sizeof(name->cstr));
}

static __inline MDBX_dbi fpta_dbicache_peek(fpta_txn *txn,
                                            const fpta_shove_t shove,
                                            const unsigned cache_hint) {
  if (likely(cache_hint < fpta_dbi_cache_size)) {
    fpta_db *db = txn->db;
    if (likely(db->dbi_shoves[cache_hint] == shove))
      return db->dbi_handles[cache_hint];
  }
  return 0;
}

static __hot MDBX_dbi fpta_dbicache_lookup(fpta_db *db, fpta_shove_t shove,
                                           unsigned *cache_hint) {
  if (*cache_hint < fpta_dbi_cache_size) {
    if (db->dbi_shoves[*cache_hint] == shove)
      return db->dbi_handles[*cache_hint];
    *cache_hint = ~0u;
  }

  const size_t n = shove % fpta_dbi_cache_size;
  size_t i = n;
  do {
    if (db->dbi_shoves[i] == shove) {
      *cache_hint = (unsigned)i;
      return db->dbi_handles[i];
    }
    i = (i + 1) % fpta_dbi_cache_size;
  } while (i != n && db->dbi_shoves[i]);

  return 0;
}

static unsigned fpta_dbicache_update(fpta_db *db, const fpta_shove_t shove,
                                     const MDBX_dbi dbi) {
  assert(shove > 0);

  const size_t n = shove % fpta_dbi_cache_size;
  size_t i = n;
  do {
    assert(db->dbi_shoves[i] != shove);
    if (db->dbi_shoves[i] == 0) {
      db->dbi_handles[i] = dbi;
      db->dbi_shoves[i] = shove;
      return (unsigned)i;
    }
    i = (i + 1) % fpta_dbi_cache_size;
  } while (i != n);

  /* TODO: прокричать что кэш переполнен (слишком много таблиц и индексов) */
  return ~0u;
}

static MDBX_dbi fpta_dbicache_remove(fpta_db *db, const fpta_shove_t shove,
                                     unsigned *const cache_hint = nullptr) {
  assert(shove > 0);

  if (cache_hint) {
    const size_t i = *cache_hint;
    if (i < fpta_dbi_cache_size) {
      *cache_hint = ~0u;
      if (db->dbi_shoves[i] == shove) {
        MDBX_dbi dbi = db->dbi_handles[i];
        db->dbi_shoves[i] = 0;
        return dbi;
      }
    }
    return 0;
  }

  const size_t n = shove % fpta_dbi_cache_size;
  size_t i = n;
  do {
    if (db->dbi_shoves[i] == shove) {
      MDBX_dbi dbi = db->dbi_handles[i];
      db->dbi_shoves[i] = 0;
      return dbi;
    }
    i = (i + 1) % fpta_dbi_cache_size;
  } while (i != n && db->dbi_shoves[i]);

  return 0;
}

static __cold int fpta_dbi_close(fpta_txn *txn, const fpta_shove_t shove,
                                 unsigned *cache_hint) {
  int rc = FPTA_SUCCESS;
  MDBX_dbi handle = fpta_dbicache_lookup(txn->db, shove, cache_hint);
  if (handle) {
    fpta_db *db = txn->db;

    if (txn->level < fpta_schema) {
      int err = fpta_mutex_lock(&db->dbi_mutex);
      if (unlikely(err != 0))
        return err;
    }

    handle = fpta_dbicache_remove(db, shove, cache_hint);
    if (likely(handle)) {
      rc = mdbx_dbi_close(db->mdbx_env, handle);
      if (rc == MDBX_BAD_DBI)
        rc = FPTA_SUCCESS;
    }

    if (txn->level < fpta_schema) {
      int err = fpta_mutex_unlock(&db->dbi_mutex);
      assert(err == 0);
      (void)err;
    }
  }

  return rc;
}

static __hot int fpta_dbi_open(fpta_txn *txn, const fpta_shove_t shove,
                               MDBX_dbi &handle, const unsigned dbi_flags,
                               const fpta_shove_t key_shove,
                               const fpta_shove_t data_shove,
                               unsigned *const cache_hint) {
  assert(fpta_txn_validate(txn, fpta_read) == FPTA_SUCCESS);
  fpta_db *db = txn->db;

  if (likely(cache_hint)) {
    handle = fpta_dbicache_lookup(db, shove, cache_hint);
    if (likely(handle))
      return FPTA_SUCCESS;
  }

  if (txn->level < fpta_schema) {
    int err = fpta_mutex_lock(&db->dbi_mutex);
    if (unlikely(err != 0))
      return err;
    if (likely(cache_hint)) {
      handle = fpta_dbicache_lookup(db, shove, cache_hint);
      if (likely(handle)) {
        err = fpta_mutex_unlock(&db->dbi_mutex);
        assert(err == 0);
        (void)err;
        return FPTA_SUCCESS;
      }
    }
  }

  fpta_dbi_name dbi_name;
  fpta_shove2str(shove, &dbi_name);

  const auto keycmp = fpta_index_shove2comparator(key_shove);
  const auto datacmp = fpta_index_shove2comparator(data_shove);
  int rc = mdbx_dbi_open_ex(txn->mdbx_txn, dbi_name.cstr, dbi_flags, &handle,
                            keycmp, datacmp);
  if (likely(rc == FPTA_SUCCESS)) {
    assert(handle != 0);
    if (cache_hint)
      *cache_hint = fpta_dbicache_update(db, shove, handle);
  } else {
    assert(handle == 0);
  }

  if (txn->level < fpta_schema) {
    int err = fpta_mutex_unlock(&db->dbi_mutex);
    assert(err == 0);
    (void)err;
  }
  return rc;
}
static __inline unsigned fpta_dbi_flags(const fpta_shove_t *shoves_defs,
                                        const size_t n) {
  const unsigned dbi_flags =
      (n == 0)
          ? fpta_index_shove2primary_dbiflags(shoves_defs[0])
          : fpta_index_shove2secondary_dbiflags(shoves_defs[0], shoves_defs[n]);
  return dbi_flags;
}

static __inline fpta_shove_t fpta_data_shove(const fpta_shove_t *shoves_defs,
                                             const size_t n) {
  const fpta_shove_t data_shove =
      n ? shoves_defs[0]
        : fpta_column_shove(0, fptu_nested,
                            fpta_primary_unique_ordered_obverse);
  return data_shove;
}

static int fpta_schema_open(fpta_txn *txn, bool create) {
  assert(fpta_txn_validate(txn, create ? fpta_schema : fpta_read) ==
         FPTA_SUCCESS);
  const fpta_shove_t key_shove =
      fpta_column_shove(0, fptu_uint64, fpta_primary_unique_ordered_obverse);
  const fpta_shove_t data_shove =
      fpta_column_shove(0, fptu_opaque, fpta_primary_unique_ordered_obverse);
  return fpta_dbi_open(txn, 0, txn->db->schema_dbi,
                       create ? MDBX_INTEGERKEY | MDBX_CREATE : MDBX_INTEGERKEY,
                       key_shove, data_shove, nullptr);
}

int __hot fpta_open_table(fpta_txn *txn, fpta_table_schema *table_def,
                          MDBX_dbi &handle) {
  const fpta_shove_t dbi_shove = fpta_dbi_shove(table_def->table_shove(), 0);
  handle = fpta_dbicache_peek(txn, dbi_shove, table_def->handle_cache(0));
  if (likely(handle > 0))
    return FPTA_OK;

  const unsigned dbi_flags =
      fpta_dbi_flags(table_def->column_shoves_array(), 0);
  const fpta_shove_t data_shove =
      fpta_data_shove(table_def->column_shoves_array(), 0);
  return fpta_dbi_open(txn, dbi_shove, handle, dbi_flags, table_def->table_pk(),
                       data_shove, &table_def->handle_cache(0));
}

int __hot fpta_open_column(fpta_txn *txn, fpta_name *column_id,
                           MDBX_dbi &tbl_handle, MDBX_dbi &idx_handle) {
  assert(fpta_id_validate(column_id, fpta_column) == FPTA_SUCCESS);

  fpta_table_schema *table_def = column_id->column.table->table_schema;
  int rc = fpta_open_table(txn, table_def, tbl_handle);
  if (unlikely(rc != FPTA_SUCCESS))
    return rc;

  if (column_id->column.num == 0) {
    idx_handle = tbl_handle;
    return FPTA_SUCCESS;
  }

  fpta_shove_t dbi_shove =
      fpta_dbi_shove(table_def->table_shove(), column_id->column.num);
  idx_handle = fpta_dbicache_peek(
      txn, dbi_shove, table_def->handle_cache(column_id->column.num));
  if (likely(idx_handle > 0))
    return FPTA_OK;

  const unsigned dbi_flags =
      fpta_dbi_flags(table_def->column_shoves_array(), column_id->column.num);
  return fpta_dbi_open(txn, dbi_shove, idx_handle, dbi_flags, column_id->shove,
                       table_def->table_pk(),
                       &table_def->handle_cache(column_id->column.num));
}

int __hot fpta_open_secondaries(fpta_txn *txn, fpta_table_schema *table_def,
                                MDBX_dbi *dbi_array) {
  int rc = fpta_open_table(txn, table_def, dbi_array[0]);
  if (unlikely(rc != FPTA_SUCCESS))
    return rc;

  for (size_t i = 1; i < table_def->column_count(); ++i) {
    const fpta_shove_t shove = table_def->column_shove(i);
    if (!fpta_is_indexed(shove))
      break;

    const fpta_shove_t dbi_shove = fpta_dbi_shove(table_def->table_shove(), i);
    const unsigned dbi_flags =
        fpta_dbi_flags(table_def->column_shoves_array(), i);
    rc = fpta_dbi_open(txn, dbi_shove, dbi_array[i], dbi_flags, shove,
                       table_def->table_pk(), &table_def->handle_cache(i));
    if (unlikely(rc != FPTA_SUCCESS))
      return rc;
  }

  return FPTA_SUCCESS;
}

//----------------------------------------------------------------------------

int fpta_column_describe(const char *column_name, enum fptu_type data_type,
                         fpta_index_type index_type,
                         fpta_column_set *column_set) {
  if (unlikely(!fpta_validate_name(column_name)))
    return FPTA_EINVAL;

  if (unlikely(data_type == fptu_null ||
               data_type == (fptu_null | fptu_farray) ||
               data_type > (fptu_nested /* TODO: | fptu_farray */)))
    return FPTA_EINVAL;

  if (fpta_is_indexed(index_type) && fpta_index_is_reverse(index_type) &&
      (!fpta_index_is_ordered(index_type) || data_type < fptu_96)) {
    if (!fpta_index_is_nullable(index_type) ||
        !fpta_nullable_reverse_sensitive(data_type))
      return FPTA_EINVAL;
  }

  switch (index_type) {
  default:
    return FPTA_EINVAL;

  case fpta_index_none:
  case fpta_noindex_nullable:

  case fpta_primary_withdups_ordered_obverse:
  case fpta_primary_withdups_ordered_obverse_nullable:
  case fpta_primary_withdups_ordered_reverse:
  case fpta_primary_withdups_ordered_reverse_nullable:

  case fpta_primary_unique_ordered_obverse:
  case fpta_primary_unique_ordered_obverse_nullable:
  case fpta_primary_unique_ordered_reverse:
  case fpta_primary_unique_ordered_reverse_nullable:

  case fpta_primary_unique_unordered:
  case fpta_primary_unique_unordered_nullable_obverse:
  case fpta_primary_unique_unordered_nullable_reverse:

  case fpta_primary_withdups_unordered:
  case fpta_primary_withdups_unordered_nullable_obverse:
  /* fpta_primary_withdups_unordered_nullable_reverse = НЕДОСТУПЕН,
   * так как битовая коминация совпадает с fpta_noindex_nullable */

  case fpta_secondary_withdups_ordered_obverse:
  case fpta_secondary_withdups_ordered_obverse_nullable:
  case fpta_secondary_withdups_ordered_reverse:
  case fpta_secondary_withdups_ordered_reverse_nullable:

  case fpta_secondary_unique_ordered_obverse:
  case fpta_secondary_unique_ordered_obverse_nullable:
  case fpta_secondary_unique_ordered_reverse:
  case fpta_secondary_unique_ordered_reverse_nullable:

  case fpta_secondary_unique_unordered:
  case fpta_secondary_unique_unordered_nullable_obverse:
  case fpta_secondary_unique_unordered_nullable_reverse:

  case fpta_secondary_withdups_unordered:
  case fpta_secondary_withdups_unordered_nullable_obverse:
  case fpta_secondary_withdups_unordered_nullable_reverse:
    assert((index_type & fpta_column_index_mask) == index_type);
    assert(index_type != (fpta_index_type)fpta_flag_table);
  }

  if (unlikely(column_set == nullptr || column_set->count > fpta_max_cols))
    return FPTA_EINVAL;

  const fpta_shove_t shove = fpta_column_shove(
      fpta_shove_name(column_name, fpta_column), data_type, index_type);
  assert(fpta_shove2index(shove) != (fpta_index_type)fpta_flag_table);

  for (size_t i = 0; i < column_set->count; ++i) {
    if (fpta_shove_eq(column_set->shoves[i], shove))
      return FPTA_EEXIST;
  }

  if (fpta_is_indexed(index_type) && fpta_index_is_primary(index_type)) {
    if (column_set->shoves[0])
      return FPTA_EEXIST;
    column_set->shoves[0] = shove;
    if (column_set->count < 1)
      column_set->count = 1;
  } else {
    if (fpta_index_is_secondary(index_type) && column_set->shoves[0] &&
        !fpta_index_is_unique(column_set->shoves[0]))
      return FPTA_EINVAL;
    if (unlikely(column_set->count == fpta_max_cols))
      return FPTA_TOOMANY;
    size_t place = (column_set->count > 0) ? column_set->count : 1;
    column_set->shoves[place] = shove;
    column_set->count = (unsigned)place + 1;
  }

  return FPTA_SUCCESS;
}

static int fpta_column_def_validate(const fpta_shove_t *def, size_t count) {
  if (unlikely(count < 1))
    return FPTA_EINVAL;
  if (unlikely(count > fpta_max_cols))
    return FPTA_TOOMANY;

  size_t index_count = 0;
  for (size_t i = 0; i < count; ++i) {
    const fpta_shove_t shove = def[i];
    const fpta_index_type index_type = fpta_shove2index(shove);
    switch (index_type) {
    default:
      return FPTA_EINVAL;

    case fpta_primary_withdups_ordered_obverse:
    case fpta_primary_withdups_ordered_obverse_nullable:
    case fpta_primary_withdups_ordered_reverse:
    case fpta_primary_withdups_ordered_reverse_nullable:

    case fpta_primary_unique_ordered_obverse:
    case fpta_primary_unique_ordered_obverse_nullable:
    case fpta_primary_unique_ordered_reverse:
    case fpta_primary_unique_ordered_reverse_nullable:

    case fpta_primary_unique_unordered:
    case fpta_primary_unique_unordered_nullable_obverse:
    case fpta_primary_unique_unordered_nullable_reverse:

    case fpta_primary_withdups_unordered:
    case fpta_primary_withdups_unordered_nullable_obverse:
      /* fpta_primary_withdups_unordered_nullable_reverse = НЕДОСТУПЕН,
       * так как битовая коминация совпадает с fpta_noindex_nullable */

      if (i != 0)
        /* первичный ключ может быть только один и только в самом
         * начале */
        return FPTA_EINVAL;
      break;

    case fpta_secondary_withdups_ordered_obverse:
    case fpta_secondary_withdups_ordered_obverse_nullable:
    case fpta_secondary_withdups_ordered_reverse:
    case fpta_secondary_withdups_ordered_reverse_nullable:

    case fpta_secondary_unique_ordered_obverse:
    case fpta_secondary_unique_ordered_obverse_nullable:
    case fpta_secondary_unique_ordered_reverse:
    case fpta_secondary_unique_ordered_reverse_nullable:

    case fpta_secondary_unique_unordered:
    case fpta_secondary_unique_unordered_nullable_obverse:
    case fpta_secondary_unique_unordered_nullable_reverse:

    case fpta_secondary_withdups_unordered:
    case fpta_secondary_withdups_unordered_nullable_obverse:
    case fpta_secondary_withdups_unordered_nullable_reverse:
      if (i > 0 && !fpta_is_indexed(def[i - 1]))
        /* сначала должны идти все индексируемые колонки, потом не
         * индексируемые */
        return FPTA_EINVAL;
      if (!fpta_index_is_unique(def[0]))
        /* для вторичных индексов первичный ключ должен быть
         * уникальным */
        return FPTA_EINVAL;
      if (++index_count > fpta_max_indexes)
        return FPTA_TOOMANY;
    // fall through
    case fpta_index_none:
    case fpta_noindex_nullable:
      if (i == 0)
        return FPTA_EINVAL;
      break;
    }
    assert((index_type & fpta_column_index_mask) == index_type);
    assert(index_type != (fpta_index_type)fpta_flag_table);

    const fptu_type data_type = fpta_shove2type(shove);
    if (data_type < fptu_uint16 || data_type == (fptu_null | fptu_farray) ||
        data_type > (fptu_nested /* TODO: | fptu_farray */))
      return false;

    if (fpta_is_indexed(index_type) && fpta_index_is_reverse(index_type) &&
        (!fpta_index_is_ordered(index_type) || data_type < fptu_96)) {
      if (!fpta_index_is_nullable(index_type) ||
          !fpta_nullable_reverse_sensitive(data_type))
        return FPTA_EINVAL;
    }

    for (size_t j = 0; j < i; ++j)
      if (fpta_shove_eq(shove, def[j]))
        return FPTA_EINVAL;
  }

  return FPTA_SUCCESS;
}

static __inline int weight(const fpta_shove_t index) {
  if (fpta_is_indexed(index))
    return 3;
  if (index & fpta_index_fnullable)
    return 1;
  return 0;
}

int fpta_column_set_validate(fpta_column_set *column_set) {
  if (column_set == nullptr)
    return FPTA_EINVAL;
  if (unlikely(column_set->count < 1))
    return FPTA_EINVAL;
  if (unlikely(column_set->count > fpta_max_cols))
    return FPTA_TOOMANY;

  /* сортируем описание колонок, так чтобы неиндексируемые были в конце */
  std::stable_sort(column_set->shoves + 1,
                   column_set->shoves + column_set->count,
                   [](const fpta_shove_t &left, const fpta_shove_t &right) {
                     const auto left_weight = weight(left);
                     const auto rigth_weight = weight(right);
                     return left_weight > rigth_weight;
                   });

  return fpta_column_def_validate(column_set->shoves, column_set->count);
}

//----------------------------------------------------------------------------

bool fpta_schema_validate(const fpta_shove_t schema_key,
                          const MDBX_val &schema_data) {
  if (unlikely(schema_data.iov_len < fpta_table_stored_schema_size(1)))
    return false;

  if (unlikely((schema_data.iov_len - sizeof(fpta_table_stored_schema)) %
               sizeof(fpta_shove_t)))
    return false;

  const fpta_table_stored_schema *schema =
      (const fpta_table_stored_schema *)schema_data.iov_base;
  if (unlikely(schema->signature != FTPA_SCHEMA_SIGNATURE))
    return false;

  if (unlikely(schema->count > fpta_max_cols))
    return false;

  if (unlikely(schema_data.iov_len !=
               fpta_table_stored_schema_size(schema->count)))
    return false;

  if (unlikely(schema->csn == 0))
    return false;

  if (unlikely(fpta_shove2index(schema_key) !=
               (fpta_index_type)fpta_flag_table))
    return false;

  uint64_t checksum =
      t1ha(&schema->signature, schema_data.iov_len - sizeof(checksum),
           FTPA_SCHEMA_CHECKSEED);
  if (unlikely(checksum != schema->checksum))
    return false;

  return FPTA_SUCCESS ==
         fpta_column_def_validate(schema->columns, schema->count);
}

static int fpta_schema_clone(const fpta_shove_t schema_key,
                             const MDBX_val &schema_data,
                             fpta_table_schema **def) {
  assert(schema_data.iov_len >= fpta_table_stored_schema_size(1) &&
         schema_data.iov_len <= sizeof(fpta_table_stored_schema));
  assert(def != nullptr);

  fpta_table_schema *schema =
      (fpta_table_schema *)realloc(*def, sizeof(fpta_table_schema));
  if (unlikely(schema == nullptr))
    return FPTA_ENOMEM;

  memset(schema, ~0, sizeof(fpta_table_schema));
  memcpy(&schema->_stored, schema_data.iov_base, schema_data.iov_len);
  schema->_key = schema_key;
  *def = schema;

  return FPTA_SUCCESS;
}

static void fpta_schema_free(fpta_table_schema *def) {
  if (likely(def)) {
    def->_stored.signature = 0;
    def->_stored.checksum = ~def->_stored.checksum;
    def->_stored.count = 0;
    free(def);
  }
}

static int fpta_schema_read(fpta_txn *txn, fpta_shove_t schema_key,
                            fpta_table_schema **def) {
  assert(fpta_txn_validate(txn, fpta_read) == FPTA_SUCCESS && def);

  int rc;
  fpta_db *db = txn->db;
  if (db->schema_dbi < 1) {
    rc = fpta_schema_open(txn, false);
    if (rc != MDBX_SUCCESS)
      return rc;
  }

  MDBX_val mdbx_data, mdbx_key;
  mdbx_key.iov_len = sizeof(schema_key);
  mdbx_key.iov_base = &schema_key;
  rc = mdbx_get(txn->mdbx_txn, db->schema_dbi, &mdbx_key, &mdbx_data);
  if (rc != MDBX_SUCCESS)
    return rc;

  if (!fpta_schema_validate(schema_key, mdbx_data))
    return FPTA_SCHEMA_CORRUPTED;

  return fpta_schema_clone(schema_key, mdbx_data, def);
}

int fpta_schema_fetch(fpta_txn *txn, fpta_schema_info *info) {
  if (!info)
    return FPTA_EINVAL;
  memset(info, 0, sizeof(fpta_schema_info));

  int rc = fpta_txn_validate(txn, fpta_read);
  if (unlikely(rc != FPTA_SUCCESS))
    return rc;

  fpta_db *db = txn->db;
  if (db->schema_dbi < 1) {
    rc = fpta_schema_open(txn, false);
    if (rc != MDBX_SUCCESS)
      return rc;
  }

  MDBX_cursor *mdbx_cursor;
  rc = mdbx_cursor_open(txn->mdbx_txn, db->schema_dbi, &mdbx_cursor);
  if (rc != MDBX_SUCCESS)
    return rc;

  MDBX_val mdbx_data, mdbx_key;
  rc = mdbx_cursor_get(mdbx_cursor, &mdbx_key, &mdbx_data, MDBX_FIRST);
  while (rc == MDBX_SUCCESS) {
    if (info->tables_count >= fpta_tables_max) {
      rc = FPTA_SCHEMA_CORRUPTED;
      break;
    }

    fpta_name *id = &info->tables_names[info->tables_count];
    if (mdbx_key.iov_len != sizeof(fpta_shove_t)) {
      rc = FPTA_SCHEMA_CORRUPTED;
      break;
    }

    memcpy(&id->shove, mdbx_key.iov_base, sizeof(id->shove));
    // id->table_schema = nullptr; /* done by memset() */
    assert(id->table_schema == nullptr);

    rc = fpta_id_validate(id, fpta_table);
    if (rc != FPTA_SUCCESS)
      break;

    if (!fpta_schema_validate(id->shove, mdbx_data)) {
      rc = FPTA_SCHEMA_CORRUPTED;
      break;
    }

    info->tables_count += 1;
    rc = mdbx_cursor_get(mdbx_cursor, &mdbx_key, &mdbx_data, MDBX_NEXT);
  }

  mdbx_cursor_close(mdbx_cursor);
  return (rc == MDBX_NOTFOUND) ? (int)FPTA_SUCCESS : rc;
}

FPTA_API int fpta_schema_destroy(fpta_schema_info *info) {
  if (unlikely(info == nullptr || info->tables_count == FPTA_DEADBEEF))
    return FPTA_EINVAL;

  for (size_t i = 0; i < info->tables_count; i++)
    fpta_name_destroy(info->tables_names + i);
  info->tables_count = (unsigned)FPTA_DEADBEEF;

  return FPTA_SUCCESS;
}

//----------------------------------------------------------------------------

static int fpta_name_init(fpta_name *id, const char *name,
                          fpta_schema_item schema_item) {
  if (unlikely(id == nullptr))
    return FPTA_EINVAL;

  memset(id, 0, sizeof(fpta_name));
  if (unlikely(!fpta_validate_name(name)))
    return FPTA_EINVAL;

  switch (schema_item) {
  default:
    return FPTA_EINVAL;
  case fpta_table:
    id->shove = fpta_shove_name(name, fpta_table);
    // id->table_schema = nullptr; /* done by memset() */
    assert(id->table_schema == nullptr);
    assert(fpta_id_validate(id, fpta_table) == FPTA_SUCCESS);
    break;
  case fpta_column:
    id->shove = fpta_column_shove(fpta_shove_name(name, fpta_column), fptu_null,
                                  fpta_index_none);
    id->column.num = ~0u;
    id->column.table = id;
    assert(fpta_id_validate(id, fpta_column) == FPTA_SUCCESS);
    break;
  }

  // id->version = 0; /* done by memset() */
  return FPTA_SUCCESS;
}

int fpta_table_init(fpta_name *table_id, const char *name) {
  return fpta_name_init(table_id, name, fpta_table);
}

int fpta_column_init(const fpta_name *table_id, fpta_name *column_id,
                     const char *name) {
  int rc = fpta_id_validate(table_id, fpta_table);
  if (unlikely(rc != FPTA_SUCCESS))
    return rc;

  rc = fpta_name_init(column_id, name, fpta_column);
  if (unlikely(rc != FPTA_SUCCESS))
    return rc;

  column_id->column.table = const_cast<fpta_name *>(table_id);
  return FPTA_SUCCESS;
}

void fpta_name_destroy(fpta_name *id) {
  if (fpta_id_validate(id, fpta_table) == FPTA_SUCCESS)
    fpta_schema_free(id->table_schema);
  memset(id, 0, sizeof(fpta_name));
}

int fpta_table_column_count_ex(const fpta_name *table_id,
                               unsigned *total_columns,
                               unsigned *composite_count) {
  int rc = fpta_id_validate(table_id, fpta_table_with_schema);
  if (unlikely(rc != FPTA_SUCCESS))
    return rc;

  const fpta_table_schema *schema = table_id->table_schema;
  if (likely(total_columns))
    *total_columns = schema->column_count();
  if (composite_count) {
    unsigned count = 0;
    for (size_t i = 0; i < schema->column_count(); ++i) {
      const auto shove = schema->column_shove(i);
      assert(i < fpta_max_indexes);
      if (!fpta_index_is_secondary(shove))
        break;
      if (fpta_shove2type(shove) == /* composite */ fptu_null)
        ++count;
    }
    *composite_count = count;
  }

  return FPTA_SUCCESS;
}

int fpta_table_column_get(const fpta_name *table_id, unsigned column,
                          fpta_name *column_id) {
  if (unlikely(column_id == nullptr))
    return FPTA_EINVAL;
  memset(column_id, 0, sizeof(fpta_name));

  int rc = fpta_id_validate(table_id, fpta_table_with_schema);
  if (unlikely(rc != FPTA_SUCCESS))
    return rc;

  const fpta_table_schema *schema = table_id->table_schema;
  if (column >= schema->column_count())
    return FPTA_NODATA;
  column_id->column.table = const_cast<fpta_name *>(table_id);
  column_id->shove = schema->column_shove(column);
  column_id->column.num = column;
  column_id->version = table_id->version;

  assert(fpta_id_validate(column_id, fpta_column_with_schema) == FPTA_SUCCESS);
  return FPTA_SUCCESS;
}

int fpta_name_reset(fpta_name *name_id) {
  if (unlikely(name_id == nullptr))
    return FPTA_EINVAL;

  name_id->version = 0;
  return FPTA_SUCCESS;
}

int fpta_name_refresh(fpta_txn *txn, fpta_name *name_id) {
  if (unlikely(name_id == nullptr))
    return FPTA_EINVAL;

  const bool is_table =
      fpta_shove2index(name_id->shove) == (fpta_index_type)fpta_flag_table;
  if (is_table)
    return fpta_name_refresh_couple(txn, name_id, nullptr);

  return fpta_name_refresh_couple(txn, name_id->column.table, name_id);
}

int fpta_name_refresh_couple(fpta_txn *txn, fpta_name *table_id,
                             fpta_name *column_id) {
  int rc = fpta_id_validate(table_id, fpta_table);
  if (unlikely(rc != FPTA_SUCCESS))
    return rc;
  if (column_id) {
    rc = fpta_id_validate(column_id, fpta_column);
    if (unlikely(rc != FPTA_SUCCESS))
      return rc;
  }
  rc = fpta_txn_validate(txn, fpta_read);
  if (unlikely(rc != FPTA_SUCCESS))
    return rc;

  if (unlikely(table_id->version > txn->schema_version()))
    return FPTA_SCHEMA_CHANGED;

  if (unlikely(table_id->version != txn->schema_version())) {
    if (table_id->table_schema) {
      rc = fpta_dbi_close(txn, table_id->shove,
                          &table_id->table_schema->handle_cache(0));
      if (unlikely(rc != FPTA_SUCCESS))
        return rc;
      for (size_t i = 1; i < table_id->table_schema->column_count(); ++i) {
        const fpta_shove_t shove = table_id->table_schema->column_shove(i);
        if (!fpta_is_indexed(shove))
          break;

        const fpta_shove_t dbi_shove = fpta_dbi_shove(table_id->shove, i);
        rc = fpta_dbi_close(txn, dbi_shove,
                            &table_id->table_schema->handle_cache(i));
        if (unlikely(rc != FPTA_SUCCESS))
          return rc;
      }
    }

    rc = fpta_schema_read(txn, table_id->shove, &table_id->table_schema);
    if (unlikely(rc != FPTA_SUCCESS)) {
      if (rc != MDBX_NOTFOUND)
        return rc;
      fpta_schema_free(table_id->table_schema);
      table_id->table_schema = nullptr;
    }

    assert(table_id->table_schema == nullptr ||
           txn->schema_version() >= table_id->table_schema->version_csn());
    table_id->version = txn->schema_version();
  }

  if (unlikely(table_id->table_schema == nullptr))
    return MDBX_NOTFOUND;

  fpta_table_schema *schema = table_id->table_schema;
  if (unlikely(schema->signature() != FTPA_SCHEMA_SIGNATURE))
    return FPTA_SCHEMA_CORRUPTED;

  assert(fpta_shove2index(table_id->shove) == (fpta_index_type)fpta_flag_table);
  if (unlikely(schema->table_shove() != table_id->shove))
    return FPTA_SCHEMA_CORRUPTED;

  assert(table_id->version >= schema->version_csn());
  if (column_id == nullptr)
    return FPTA_SUCCESS;

  assert(fpta_shove2index(column_id->shove) !=
         (fpta_index_type)fpta_flag_table);

  if (unlikely(column_id->column.table != table_id)) {
    if (column_id->column.table != column_id)
      return FPTA_EINVAL;
    column_id->column.table = table_id;
  }

  if (unlikely(column_id->version > table_id->version))
    return FPTA_SCHEMA_CHANGED;

  if (column_id->version != table_id->version) {
    column_id->column.num = ~0u;
    for (size_t i = 0; i < schema->column_count(); ++i) {
      if (fpta_shove_eq(column_id->shove, schema->column_shove(i))) {
        column_id->shove = schema->column_shove(i);
        column_id->column.num = (unsigned)i;
        break;
      }
    }
    column_id->version = table_id->version;
  }

  if (unlikely(column_id->column.num > fpta_max_cols))
    return FPTA_ENOENT;
  return FPTA_SUCCESS;
}

//----------------------------------------------------------------------------

int fpta_table_create(fpta_txn *txn, const char *table_name,
                      fpta_column_set *column_set) {
  int rc = fpta_txn_validate(txn, fpta_schema);
  if (unlikely(rc != FPTA_SUCCESS))
    return rc;
  if (!fpta_validate_name(table_name))
    return FPTA_EINVAL;

  rc = fpta_column_set_validate(column_set);
  if (rc != FPTA_SUCCESS)
    return rc;

  fpta_db *db = txn->db;
  if (db->schema_dbi < 1) {
    rc = fpta_schema_open(txn, true);
    if (rc != MDBX_SUCCESS)
      return rc;
  }

  MDBX_dbi dbi[fpta_max_indexes];
  memset(dbi, 0, sizeof(dbi));
  fpta_shove_t table_shove = fpta_shove_name(table_name, fpta_table);

  for (size_t i = 0; i < column_set->count; ++i) {
    const auto shove = column_set->shoves[i];
    if (!fpta_is_indexed(shove))
      break;
    assert(i < fpta_max_indexes);

    const unsigned dbi_flags = fpta_dbi_flags(column_set->shoves, i);
    const fpta_shove_t data_shove = fpta_data_shove(column_set->shoves, i);
    int err = fpta_dbi_open(txn, fpta_dbi_shove(table_shove, i), dbi[i],
                            dbi_flags, shove, data_shove, nullptr);
    if (err != MDBX_NOTFOUND)
      return FPTA_EEXIST;
  }

  for (size_t i = 0; i < column_set->count; ++i) {
    const auto shove = column_set->shoves[i];
    if (!fpta_is_indexed(shove))
      break;
    assert(i < fpta_max_indexes);

    const unsigned dbi_flags =
        MDBX_CREATE | fpta_dbi_flags(column_set->shoves, i);
    const fpta_shove_t data_shove = fpta_data_shove(column_set->shoves, i);
    rc = fpta_dbi_open(txn, fpta_dbi_shove(table_shove, i), dbi[i], dbi_flags,
                       shove, data_shove, nullptr);
    if (rc != MDBX_SUCCESS)
      goto bailout;
  }

  fpta_table_stored_schema stored_schema;
  MDBX_val data;
  data.iov_base = &stored_schema;
  data.iov_len = fpta_table_stored_schema_size(column_set->count);

  stored_schema.signature = FTPA_SCHEMA_SIGNATURE;
  stored_schema.count = column_set->count;
  stored_schema.csn = txn->db_version;
  memcpy(stored_schema.columns, column_set->shoves,
         sizeof(fpta_shove_t) * stored_schema.count);
  stored_schema.checksum = t1ha(&stored_schema.signature,
                                data.iov_len - sizeof(stored_schema.checksum),
                                FTPA_SCHEMA_CHECKSEED);

  MDBX_val key;
  key.iov_len = sizeof(table_shove);
  key.iov_base = &table_shove;
  rc = mdbx_put(txn->mdbx_txn, db->schema_dbi, &key, &data, MDBX_NOOVERWRITE);
  if (rc != MDBX_SUCCESS)
    goto bailout;

  txn->schema_version() = txn->db_version;
  return FPTA_SUCCESS;

bailout:
  for (size_t i = 0; i < fpta_max_indexes && dbi[i] > 0; ++i) {
    fpta_dbicache_remove(db, fpta_dbi_shove(table_shove, i));
    int err = mdbx_drop(txn->mdbx_txn, dbi[i], true);
    if (unlikely(err != MDBX_SUCCESS))
      return fpta_internal_abort(txn, err);
  }
  return rc;
}

int fpta_table_drop(fpta_txn *txn, const char *table_name) {
  int rc = fpta_txn_validate(txn, fpta_schema);
  if (unlikely(rc != FPTA_SUCCESS))
    return rc;
  if (!fpta_validate_name(table_name))
    return FPTA_EINVAL;

  fpta_db *db = txn->db;
  if (db->schema_dbi < 1) {
    rc = fpta_schema_open(txn, true);
    if (rc != MDBX_SUCCESS)
      return rc;
  }

  MDBX_dbi dbi[fpta_max_indexes];
  memset(dbi, 0, sizeof(dbi));
  fpta_shove_t schema_key = fpta_shove_name(table_name, fpta_table);

  MDBX_val data, key;
  key.iov_len = sizeof(schema_key);
  key.iov_base = &schema_key;
  rc = mdbx_get(txn->mdbx_txn, db->schema_dbi, &key, &data);
  if (rc != MDBX_SUCCESS)
    return rc;

  if (!fpta_schema_validate(schema_key, data))
    return FPTA_SCHEMA_CORRUPTED;

  const fpta_table_stored_schema *const stored_schema =
      (const fpta_table_stored_schema *)data.iov_base;
  for (size_t i = 0; i < stored_schema->count; ++i) {
    const auto shove = stored_schema->columns[i];
    if (!fpta_is_indexed(shove))
      break;
    assert(i < fpta_max_indexes);

    const unsigned dbi_flags = fpta_dbi_flags(stored_schema->columns, i);
    const fpta_shove_t data_shove = fpta_data_shove(stored_schema->columns, i);
    rc = fpta_dbi_open(txn, fpta_dbi_shove(schema_key, i), dbi[i], dbi_flags,
                       shove, data_shove, nullptr);
    if (rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND)
      return rc;
  }

  rc = mdbx_del(txn->mdbx_txn, db->schema_dbi, &key, nullptr);
  if (rc != MDBX_SUCCESS)
    return rc;

  txn->schema_version() = txn->db_version;
  for (size_t i = 0; i < stored_schema->count; ++i) {
    if (dbi[i] > 0) {
      fpta_dbicache_remove(db, fpta_dbi_shove(schema_key, i));
      int err = mdbx_drop(txn->mdbx_txn, dbi[i], true);
      if (unlikely(err != MDBX_SUCCESS))
        return fpta_internal_abort(txn, err);
    }
  }

  return rc;
}

//----------------------------------------------------------------------------

int fpta_check_notindexed_cols(const fpta_table_schema *table_def,
                               const fptu_ro &row) {
  assert(table_def->column_count() > 0);
  for (size_t i = table_def->column_count(); --i > 0;) {
    const auto shove = table_def->column_shove(i);
    const auto index = fpta_shove2index(shove);
    assert(i < fpta_max_indexes);
    if (index > fpta_index_none) {
      assert(fpta_index_is_secondary(index) || (index & fpta_index_fnullable));
#ifdef NDEBUG
      break;
#endif
    } else {
      const fptu_type type = fpta_shove2type(shove);
      const fptu_field *field = fptu_lookup_ro(row, (unsigned)i, type);
      if (unlikely(field == nullptr))
        return FPTA_COLUMN_MISSING;
    }
  }
  return FPTA_SUCCESS;
}
