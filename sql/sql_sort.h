#ifndef SQL_SORT_INCLUDED
#define SQL_SORT_INCLUDED

/* Copyright (c) 2000, 2010, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2020, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "my_base.h"                            /* ha_rows */
#include <my_sys.h>                             /* qsort2_cmp */
#include "queues.h"
#include "sql_class.h"

class Field;
struct TABLE;

/* Defines used by filesort and uniques */

#define MERGEBUFF		7
#define MERGEBUFF2		15

/*
   The structure SORT_ADDON_FIELD describes a fixed layout
   for field values appended to sorted values in records to be sorted
   in the sort buffer.
   Only fixed layout is supported now.
   Null bit maps for the appended values is placed before the values 
   themselves. Offsets are from the last sorted field, that is from the
   record referefence, which is still last component of sorted records.
   It is preserved for backward compatiblility.
   The structure is used tp store values of the additional fields 
   in the sort buffer. It is used also when these values are read
   from a temporary file/buffer. As the reading procedures are beyond the
   scope of the 'filesort' code the values have to be retrieved via
   the callback function 'unpack_addon_fields'.
*/

typedef struct st_sort_addon_field
{
  /* Sort addon packed field */
  Field *field;          /* Original field */
  uint   offset;         /* Offset from the last sorted field */
  uint   null_offset;    /* Offset to to null bit from the last sorted field */
  uint   length;         /* Length in the sort buffer */
  uint8  null_bit;       /* Null bit mask for the field */
} SORT_ADDON_FIELD;

struct BUFFPEK_COMPARE_CONTEXT
{
  qsort_cmp2 key_compare;
  void *key_compare_arg;
};


/**
  Descriptor for a merge chunk to be sort-merged.
  A merge chunk is a sequence of pre-sorted records, written to a
  temporary file. A Merge_chunk instance describes where this chunk is stored
  in the file, and where it is located when it is in memory.

  It is a POD because
   - we read/write them from/to files.

  We have accessors (getters/setters) for all struct members.
 */

struct Merge_chunk {
public:
  my_off_t file_position() const { return m_file_position; }
  void set_file_position(my_off_t val) { m_file_position= val; }
  void advance_file_position(my_off_t val) { m_file_position+= val; }

  uchar *buffer_start() { return m_buffer_start; }
  const uchar *buffer_end() const { return m_buffer_end; }

  void set_buffer(uchar *start, uchar *end)
  {
    m_buffer_start= start;
    m_buffer_end= end;
  }
  void set_buffer_start(uchar *start)
  {
    m_buffer_start= start;
  }
  void set_buffer_end(uchar *end)
  {
    DBUG_ASSERT(m_buffer_end == NULL || end <= m_buffer_end);
    m_buffer_end= end;
  }

  void init_current_key() { m_current_key= m_buffer_start; }
  uchar *current_key() { return m_current_key; }
  void advance_current_key(uint val) { m_current_key+= val; }

  void decrement_rowcount(ha_rows val) { m_rowcount-= val; }
  void set_rowcount(ha_rows val)       { m_rowcount= val; }
  ha_rows rowcount() const             { return m_rowcount; }

  ha_rows mem_count() const { return m_mem_count; }
  void set_mem_count(ha_rows val) { m_mem_count= val; }
  ha_rows decrement_mem_count() { return --m_mem_count; }

  ha_rows max_keys() const { return m_max_keys; }
  void set_max_keys(ha_rows val) { m_max_keys= val; }

  size_t  buffer_size() const { return m_buffer_end - m_buffer_start; }

  /**
    Tries to merge *this with *mc, returns true if successful.
    The assumption is that *this is no longer in use,
    and the space it has been allocated can be handed over to a
    buffer which is adjacent to it.
   */
  bool merge_freed_buff(Merge_chunk *mc) const
  {
    if (mc->m_buffer_end == m_buffer_start)
    {
      mc->m_buffer_end= m_buffer_end;
      mc->m_max_keys+= m_max_keys;
      return true;
    }
    else if (mc->m_buffer_start == m_buffer_end)
    {
      mc->m_buffer_start= m_buffer_start;
      mc->m_max_keys+= m_max_keys;
      return true;
    }
    return false;
  }

  /// The current key for this chunk
  uchar *m_current_key= nullptr;
  /// Current position in the file to be sorted.
  my_off_t m_file_position= 0;
  /// Start of main-memory buffer for this chunk.
  uchar *m_buffer_start= nullptr;
  /// End of main-memory buffer for this chunk.
  uchar *m_buffer_end= nullptr;
  /// Number of unread rows in this chunk.
  ha_rows m_rowcount= 0;
  /// Number of rows in the main-memory buffer.
  ha_rows m_mem_count= 0;
  /// If we have fixed-size rows: max number of rows in buffer.
  ha_rows m_max_keys= 0;
};

typedef Bounds_checked_array<SORT_ADDON_FIELD> Addon_fields_array;
typedef Bounds_checked_array<SORT_FIELD> Sort_keys_array;

/**
  This class wraps information about usage of addon fields.
  An Addon_fields object is used both during packing of data in the filesort
  buffer, and later during unpacking in 'Filesort_info::unpack_addon_fields'.

  @see documentation for the Sort_addon_field struct.
  @see documentation for get_addon_fields()
 */
class Addon_fields {
public:
  Addon_fields(Addon_fields_array arr)
    : m_field_descriptors(arr),
      m_addon_buf(),
      m_addon_buf_length(),
      m_using_packed_addons(false)
  {
    DBUG_ASSERT(!arr.is_null());
  }

  SORT_ADDON_FIELD *begin() { return m_field_descriptors.begin(); }
  SORT_ADDON_FIELD *end()   { return m_field_descriptors.end(); }

    /// rr_unpack_from_tempfile needs an extra buffer when unpacking.
  uchar *allocate_addon_buf(uint sz)
  {
    m_addon_buf= (uchar *)my_malloc(sz, MYF(MY_WME | MY_THREAD_SPECIFIC));
    if (m_addon_buf)
      m_addon_buf_length= sz;
    return m_addon_buf;
  }

  void free_addon_buff()
  {
    my_free(m_addon_buf);
    m_addon_buf= NULL;
    m_addon_buf_length= 0;
  }

  uchar *get_addon_buf() { return m_addon_buf; }
  uint   get_addon_buf_length() const { return m_addon_buf_length; }

  void set_using_packed_addons(bool val)
  {
    m_using_packed_addons= val;
  }

  bool using_packed_addons() const
  {
    return m_using_packed_addons;
  }

  static bool can_pack_addon_fields(uint record_length)
  {
    return (record_length <= (0xFFFF));
  }

  /**
    @returns Total number of bytes used for packed addon fields.
    the size of the length field + size of null bits + sum of field sizes.
   */
  static uint read_addon_length(uchar *p)
  {
    return size_of_length_field + uint2korr(p);
  }

  /**
    Stores the number of bytes used for packed addon fields.
   */
  static void store_addon_length(uchar *p, uint sz)
  {
    // We actually store the length of everything *after* the length field.
    int2store(p, sz - size_of_length_field);
  }

  static const uint size_of_length_field= 2;

private:
  Addon_fields_array m_field_descriptors;

  uchar    *m_addon_buf;            ///< Buffer for unpacking addon fields.
  uint      m_addon_buf_length;     ///< Length of the buffer.
  bool      m_using_packed_addons;  ///< Are we packing the addon fields?
};

/**
  This class wraps information about usage of sort keys.
  A Sort_keys object is used both during packing of data in the filesort
  buffer, and later during unpacking in 'Filesort_info::unpack_addon_fields'.

  @see SORT_FIELD struct.
*/

class Sort_keys :public Sql_alloc,
                 public Sort_keys_array
{
public:
  Sort_keys(SORT_FIELD* arr, size_t count):
  Sort_keys_array(arr, count),
  m_using_packed_sortkeys(false),
  size_of_packable_fields(0),
  sort_length(0)
  {
    DBUG_ASSERT(!is_null());
  }

  bool using_packed_sortkeys() const
  { return m_using_packed_sortkeys; }

  void set_using_packed_sortkeys(bool val)
  {
    m_using_packed_sortkeys= val;
  }
  void set_size_of_packable_fields(uint len)
  {
    size_of_packable_fields= len;
  }

  uint get_size_of_packable_fields()
  {
    return size_of_packable_fields;
  }

  void set_sort_length(uint len)
  {
    sort_length= len;
  }

  uint get_sort_length()
  {
    return sort_length;
  }

  static void store_sortkey_length(uchar *p, uint sz)
  {
    int4store(p, sz - size_of_length_field);
  }

  static uint read_sortkey_length(uchar *p)
  {
    return size_of_length_field + uint4korr(p);
  }

  void increment_size_of_packable_fields(uint len)
  {
    size_of_packable_fields+= len;
  }

  void increment_original_sort_length(uint len)
  {
    sort_length+= len;
  }

  static const uint size_of_length_field= 4;

private:
  bool m_using_packed_sortkeys;     // Are we packing sort keys
  uint size_of_packable_fields;     // Total length bytes for packable columns

  /*
    The length that would be needed if we stored non-packed mem-comparable
    images of fields?
  */
  uint sort_length;
};


/**
  The sort record format may use one of two formats for the non-sorted part of
  the record:

  1. Use the rowid

    |<sort_key>|   <rowid>  |
    /          / ref_length /

  2. Use "addon fields"

    |<sort_key>|<null bits>|<field a><field b>...|
    /          /         addon_length            /

  The packed format for "addon fields"

    |<sort_key>|<length>|<null bits>|<field a><field b>...|
    /          /         addon_length                     /

  <sort_key>  The key may use one of the two formats:
              A. fixed-size mem-comparable form. The record is always
                 sort_length bytes long.
              B. "PackedKeyFormat" - the records are variable-size.

  <key>       Fields are fixed-size, specially encoded with
              Field::make_sort_key() so we can do byte-by-byte compare.

  <length>    Contains the *actual* packed length (after packing) of
              everything after the sort keys.
              The size of the length field is 2 bytes,
              which should cover most use cases: addon data <= 65535 bytes.
              This is the same as max record size in MySQL.
  <null bits> One bit for each nullable field, indicating whether the field
              is null or not. May have size zero if no fields are nullable.
  <field xx>  Are stored with field->pack(), and retrieved with
              field->unpack(). Addon fields within a record are stored
              consecutively, with no "holes" or padding. They will have zero
              size for NULL values.

*/

class Sort_param {
public:
  uint rec_length;            // Length of sorted records.
  uint sort_length;           // Length of sorted columns.
  uint ref_length;            // Length of record ref.
  uint addon_length;          // Length of addon_fields
  uint res_length;            // Length of records in final sorted file/buffer.
  uint max_keys_per_buffer;   // Max keys / buffer.
  uint min_dupl_count;
  ha_rows max_rows;           // Select limit, or HA_POS_ERROR if unlimited.
  ha_rows examined_rows;      // Number of examined rows.
  TABLE *sort_form;           // For quicker make_sortkey.
  /**
    ORDER BY list with some precalculated info for filesort.
    Array is created and owned by a Filesort instance.
   */
  Bounds_checked_array<SORT_FIELD> local_sortorder;
  Addon_fields *addon_fields;     // Descriptors for companion fields.
  Sort_keys *sort_keys;
  bool using_pq;

  uchar *unique_buff;
  bool not_killable;
  char* tmp_buffer;
  // The fields below are used only by Unique class.
  qsort2_cmp compare;
  BUFFPEK_COMPARE_CONTEXT cmp_context;

  Sort_param()
  {
    memset(reinterpret_cast<void*>(this), 0, sizeof(*this));
  }
  void init_for_filesort(uint sortlen, TABLE *table,
                         ha_rows maxrows, bool sort_positions);
  /// Enables the packing of addons if possible.
  void try_to_pack_addons(ulong max_length_for_sort_data);

  /// Are we packing the "addon fields"?
  bool using_packed_addons() const
  {
    DBUG_ASSERT(m_using_packed_addons ==
                (addon_fields != NULL &&
                 addon_fields->using_packed_addons()));
    return m_using_packed_addons;
  }

  bool using_packed_sortkeys() const
  {
    DBUG_ASSERT(m_using_packed_sortkeys ==
                (sort_keys != NULL && sort_keys->using_packed_sortkeys()));
    return m_using_packed_sortkeys;
  }

  /// Are we using "addon fields"?
  bool using_addon_fields() const
  {
    return addon_fields != NULL;
  }

  uint32 get_result_length(uchar *plen)
  {
    if (!using_addon_fields())
      return res_length;
    return get_addon_length(plen);
  }

  uint32 get_addon_length(uchar *plen)
  {
    if (using_packed_addons())
      return Addon_fields::read_addon_length(plen);
    else
      return addon_length;
  }

  uint32 get_sort_length(uchar *plen)
  {
    if (using_packed_sortkeys())
      return Sort_keys::read_sortkey_length(plen) +
              /*
                when addon fields are not present, then the sort_length also
                includes the res_length. For packed keys here we add
                the res_length
              */
             (using_addon_fields() ? 0: res_length);
    else
      return sort_length;
  }

  uint get_record_length(uchar *plen)
  {
    uint sort_length= get_sort_length(plen);
    return sort_length + get_addon_length(plen + sort_length);
  }

  /**
    Getter for record length and result length.
    @param record_start Pointer to record.
    @param [out] recl   Store record length here.
    @param [out] resl   Store result length here.
   */
  void get_rec_and_res_len(uchar *record_start, uint *recl, uint *resl)
  {
    if (m_using_packed_addons || m_using_packed_sortkeys)
    {
      uint sort_length= get_sort_length(record_start);
      uint addon_length= get_addon_length(record_start + sort_length);
      *recl= sort_length + addon_length;
      *resl= using_addon_fields() ? addon_length : res_length;
    }
    else
    {
      *recl= rec_length;
      *resl= res_length;
    }
  }

  void try_to_pack_sortkeys();

  qsort2_cmp get_compare_function() const
  {
    return using_packed_sortkeys() ?
           get_packed_keys_compare_ptr() :
           get_ptr_compare(sort_length);
  }
  void* get_compare_argument(size_t *sort_len) const
  {
    return using_packed_sortkeys() ?
           (void*) this :
           (void*) sort_len;
  }

  bool is_packed_format() const
  {
    return m_packed_format;
  }

private:
  uint m_packable_length;
  bool m_using_packed_addons; ///< caches the value of using_packed_addons()
  /* caches the value of using_packed_sortkeys() */
  bool m_using_packed_sortkeys;
  bool m_packed_format;
};

typedef Bounds_checked_array<uchar> Sort_buffer;

int merge_many_buff(Sort_param *param, Sort_buffer sort_buffer,
                    Merge_chunk *buffpek, uint *maxbuffer, IO_CACHE *t_file);
ulong read_to_buffer(IO_CACHE *fromfile, Merge_chunk *buffpek,
                     Sort_param *param, bool packing_format);
bool merge_buffers(Sort_param *param,IO_CACHE *from_file,
                   IO_CACHE *to_file, Sort_buffer sort_buffer,
                   Merge_chunk *lastbuff, Merge_chunk *Fb,
                   Merge_chunk *Tb, int flag);
int merge_index(Sort_param *param, Sort_buffer sort_buffer,
                Merge_chunk *buffpek, uint maxbuffer,
                IO_CACHE *tempfile, IO_CACHE *outfile);
void reuse_freed_buff(QUEUE *queue, Merge_chunk *reuse, uint key_length);

#endif /* SQL_SORT_INCLUDED */
