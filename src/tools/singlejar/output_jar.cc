// Copyright 2016 The Bazel Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*
 * The implementation of the OutputJar methods.
 */
#include "src/tools/singlejar/output_jar.h"

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/main/cpp/util/file.h"
#include "src/tools/singlejar/combiners.h"
#include "src/tools/singlejar/diag.h"
#include "src/tools/singlejar/input_jar.h"
#include "src/tools/singlejar/mapped_file.h"
#include "src/tools/singlejar/options.h"
#include "src/tools/singlejar/zip_headers.h"

#include <zlib.h>

#define TODO(cond, msg)                                              \
  if (!(cond)) {                                                     \
    diag_errx(2, "%s:%d: TODO(asmundak): " msg, __FILE__, __LINE__); \
  }

OutputJar::OutputJar()
    : options_(nullptr),
      file_(nullptr),
      outpos_(0),
      buffer_(nullptr),
      entries_(0),
      duplicate_entries_(0),
      cen_(nullptr),
      cen_size_(0),
      cen_capacity_(0),
      spring_handlers_("META-INF/spring.handlers"),
      spring_schemas_("META-INF/spring.schemas"),
      protobuf_meta_handler_("protobuf.meta", false),
      manifest_("META-INF/MANIFEST.MF"),
      build_properties_("build-data.properties") {
  known_members_.emplace(spring_handlers_.filename(),
                         EntryInfo{&spring_handlers_});
  known_members_.emplace(spring_schemas_.filename(),
                         EntryInfo{&spring_schemas_});
  known_members_.emplace(manifest_.filename(), EntryInfo{&manifest_});
  known_members_.emplace(protobuf_meta_handler_.filename(),
                         EntryInfo{&protobuf_meta_handler_});
  manifest_.Append(
      "Manifest-Version: 1.0\r\n"
      "Created-By: singlejar\r\n");
}

int OutputJar::Doit(Options *options) {
  if (nullptr != options_) {
    diag_errx(1, "%s:%d: Doit() can be called only once.", __FILE__, __LINE__);
  }
  options_ = options;

  // Register the handler for the build-data.properties file unless
  // --exclude_build_data is present. Otherwise we do not generate this file,
  // and it will be copied from the first source archive containing it.
  if (!options_->exclude_build_data) {
    known_members_.emplace(build_properties_.filename(),
                           EntryInfo{&build_properties_});
  }

  build_properties_.AddProperty("build.target", options_->output_jar.c_str());
  if (options_->verbose) {
    fprintf(stderr, "combined_file_name=%s\n", options_->output_jar.c_str());
    if (!options_->main_class.empty()) {
      fprintf(stderr, "main_class=%s\n", options_->main_class.c_str());
    }
    if (!options_->java_launcher.empty()) {
      fprintf(stderr, "java_launcher_file=%s\n",
              options_->java_launcher.c_str());
    }
    fprintf(stderr, "%ld source files\n", options_->input_jars.size());
    fprintf(stderr, "%ld manifest lines\n", options_->manifest_lines.size());
  }

  if (!Open()) {
    exit(1);
  }

  // Copy launcher if it is set.
  if (!options_->java_launcher.empty()) {
    const char *const launcher_path = options_->java_launcher.c_str();
    int in_fd = open(launcher_path, O_RDONLY);
    struct stat statbuf;
    if (file_ == nullptr || fstat(in_fd, &statbuf)) {
      diag_err(1, "%s", launcher_path);
    }
    // TODO(asmundak):  Consider going back to sendfile() or reflink
    // (BTRFS_IOC_CLONE/XFS_IOC_CLONE) here.  The launcher preamble can
    // be very large for targets with many native deps.
    ssize_t byte_count = AppendFile(in_fd, 0, statbuf.st_size);
    if (byte_count < 0) {
      diag_err(1, "%s:%d: Cannot copy %s to %s", __FILE__, __LINE__,
               launcher_path, options_->output_jar.c_str());
    } else if (byte_count != statbuf.st_size) {
      diag_err(1, "%s:%d: Copied only %ld bytes out of %" PRIu64 " from %s",
               __FILE__, __LINE__, byte_count, statbuf.st_size, launcher_path);
    }
    close(in_fd);
    if (options_->verbose) {
      fprintf(stderr, "Prepended %s (%" PRIu64 " bytes)\n", launcher_path,
              statbuf.st_size);
    }
  }

  if (!options_->main_class.empty()) {
    build_properties_.AddProperty("main.class", options_->main_class);
    manifest_.Append("Main-Class: ");
    manifest_.Append(options_->main_class);
    manifest_.Append("\r\n");
  }

  for (auto &manifest_line : options_->manifest_lines) {
    if (!manifest_line.empty()) {
      manifest_.Append(manifest_line);
      if (manifest_line[manifest_line.size() - 1] != '\n') {
        manifest_.Append("\r\n");
      }
    }
  }

  for (auto &build_info_line : options_->build_info_lines) {
    build_properties_.Append(build_info_line);
    build_properties_.Append("\n");
  }

  for (auto &build_info_file : options_->build_info_files) {
    MappedFile mapped_file;
    if (!mapped_file.Open(build_info_file)) {
      diag_err(1, "%s:%d: Bad build info file %s", __FILE__, __LINE__,
               build_info_file.c_str());
    }
    const char *data = reinterpret_cast<const char *>(mapped_file.start());
    const char *data_end = reinterpret_cast<const char *>(mapped_file.end());
    // TODO(asmundak): this isn't right, we should parse properties file.
    while (data < data_end) {
      const char *next_data = strchr(static_cast<const char *>(data), '\n');
      if (next_data) {
        ++next_data;
      } else {
        next_data = data_end;
      }
      build_properties_.Append(data, next_data - data);
      data = next_data;
    }
    mapped_file.Close();
  }

  for (auto &rpath : options_->classpath_resources) {
    ClasspathResource(blaze_util::Basename(rpath), rpath);
  }

  for (auto &rdesc : options_->resources) {
    // A resource description is either NAME or PATH:NAME
    std::size_t colon = rdesc.find_first_of(':');
    if (0 == colon) {
      diag_errx(1, "%s:%d: Bad resource description %s", __FILE__, __LINE__,
                rdesc.c_str());
    }
    if (std::string::npos == colon) {
      ClasspathResource(rdesc, rdesc);
    } else {
      ClasspathResource(rdesc.substr(colon + 1), rdesc.substr(0, colon));
    }
  }

  // Ready to write zip entries. Decide whether created entries should be
  // compressed.
  bool compress = options_->force_compression || options_->preserve_compression;
  // First, write a directory entry for the META-INF, followed by the manifest
  // file, followed by the build properties file.
  AddDirectory("META-INF/");
  manifest_.Append("\r\n");
  WriteEntry(manifest_.OutputEntry(compress));
  if (!options_->exclude_build_data) {
    WriteEntry(build_properties_.OutputEntry(compress));
  }

  // Then classpath resources.
  for (auto &classpath_resource : classpath_resources_) {
    WriteEntry(classpath_resource->OutputEntry(compress));
  }

  // Then copy source files' contents.
  for (int ix = 0; ix < options_->input_jars.size(); ++ix) {
    if (!AddJar(ix)) {
      exit(1);
    }
  }

  // All entries written, write Central Directory and close.
  Close();
  return 0;
}

OutputJar::~OutputJar() {
  if (file_) {
    diag_warnx("%s:%d: Close() should be called first", __FILE__, __LINE__);
  }
}

// Try to perform I/O in units of this size.
// (128KB is the default max request size for fuse filesystems.)
static const size_t kBufferSize = 128<<10;

bool OutputJar::Open() {
  if (file_) {
    diag_errx(1, "%s:%d: Cannot open output archive twice", __FILE__, __LINE__);
  }
  // Set execute bits since we may produce an executable output file.
  int fd = open(path(), O_CREAT|O_WRONLY|O_TRUNC, 0777);
  if (fd < 0) {
    diag_warn("%s:%d: %s", __FILE__, __LINE__, path());
    return false;
  }
  file_ = fdopen(fd, "w");
  if (file_ == nullptr) {
    diag_warn("%s:%d: fdopen of %s", __FILE__, __LINE__, path());
    close(fd);
    return false;
  }
  outpos_ = 0;
  buffer_.reset(new char[kBufferSize]);
  setbuffer(file_, buffer_.get(), kBufferSize);
  if (options_->verbose) {
    fprintf(stderr, "Writing to %s\n", path());
  }
  return true;
}

bool OutputJar::AddJar(int jar_path_index) {
  const std::string& input_jar_path = options_->input_jars[jar_path_index];
  InputJar input_jar;
  if (!input_jar.Open(input_jar_path)) {
    return false;
  }
  const CDH *jar_entry;
  const LH *lh;
  while ((jar_entry = input_jar.NextEntry(&lh))) {
    const char *file_name = jar_entry->file_name();
    auto file_name_length = jar_entry->file_name_length();
    if (!file_name_length) {
      diag_errx(
          1, "%s:%d: Bad central directory record in %s at offset 0x%" PRIx64,
          __FILE__, __LINE__, input_jar_path.c_str(),
          input_jar.CentralDirectoryRecordOffset(jar_entry));
    }
    // Special files that cannot be handled by looking up known_members_ map:
    // * ignore *.SF, *.RSA, *.DSA
    //   (TODO(asmundak): should this be done only in META-INF?
    //
    if (ends_with(file_name, file_name_length, ".SF") ||
        ends_with(file_name, file_name_length, ".RSA") ||
        ends_with(file_name, file_name_length, ".DSA")) {
      continue;
    }

    bool include_entry = true;
    if (!options_->include_prefixes.empty()) {
      for (auto& prefix : options_->include_prefixes) {
        if ((include_entry =
                 (prefix.size() <= file_name_length &&
                  0 == strncmp(file_name, prefix.c_str(), prefix.size())))) {
          break;
        }
      }
    }
    if (!include_entry) {
      continue;
    }

    bool is_file = (file_name[file_name_length - 1] != '/');
    if (is_file &&
        begins_with(file_name, file_name_length, "META-INF/services/")) {
      // The contents of the META-INF/services/<SERVICE> on the output is the
      // concatenation of the META-INF/services/<SERVICE> files from all inputs.
      std::string service_path(file_name, file_name_length);
      if (NewEntry(service_path)) {
        // Create a concatenator and add it to the known_members_ map.
        // The call to Merge() below will then take care of the rest.
        Concatenator *service_handler = new Concatenator(service_path);
        service_handlers_.emplace_back(service_handler);
        known_members_.emplace(service_path, EntryInfo{service_handler});
      }
    } else {
      ExtraHandler(jar_entry);
    }

    // Install a new entry unless it is already present. All the plain (non-dir)
    // entries that require a combiner have been already installed, so the call
    // will add either a directory entry whose handler will ignore subsequent
    // duplicates, or an ordinary plain entry, for which we save the index of
    // the first input jar (in order to provide diagnostics on duplicate).
    auto got =
        known_members_.emplace(std::string(file_name, file_name_length),
                               EntryInfo{is_file ? nullptr : &null_combiner_,
                                         is_file ? jar_path_index: -1});
    if (!got.second) {
      auto &entry_info = got.first->second;
      // Handle special entries (the ones that have a combiner).
      if (entry_info.combiner_ != nullptr) {
        entry_info.combiner_->Merge(jar_entry, lh);
        continue;
      }

      // Plain file entry. If duplicates are not allowed, bail out. Otherwise
      // just ignore this entry.
      if (options_->no_duplicates ||
          (options_->no_duplicate_classes &&
           ends_with(file_name, file_name_length, ".class"))) {
        diag_errx(1, "%s:%d: %.*s is present both in %s and %s", __FILE__,
                  __LINE__, file_name_length, file_name,
                  options_->input_jars[entry_info.input_jar_index_].c_str(),
                  input_jar_path.c_str());
      } else {
        duplicate_entries_++;
        continue;
      }
    }

    // For the file entries and unless preserve_compression option is set,
    // decide what to do with an entry depending on force_compress option
    // and entry's current state:
    //   force_compress    preserve_compress   compressed    Action
    //         N                  N                 N        Copy
    //         N                  N                 Y        Decompress
    //         N                  Y                 *        Copy
    //         Y                  *                 N        Compress
    //         Y                  N                 Y        Copy
    //         Y                  Y      can't be
    if (is_file &&
        !options_->preserve_compression &&
        ((options_->force_compression &&
          jar_entry->compression_method() == Z_NO_COMPRESSION) ||
         (!options_->force_compression && !options_->preserve_compression &&
          jar_entry->compression_method() == Z_DEFLATED))) {
      // Change compression.
      Concatenator combiner(jar_entry->file_name_string());
      if (!combiner.Merge(jar_entry, lh)) {
        diag_err(1, "%s:%d: cannot add %.*s", __FILE__, __LINE__,
                 jar_entry->file_name_length(), jar_entry->file_name());
      }
      WriteEntry(combiner.OutputEntry(options_->force_compression));
      continue;
    }

    // Now we have to copy:
    //  local header
    //  file data
    //  data descriptor, if present.
    off_t copy_from = jar_entry->local_header_offset();
    size_t num_bytes = lh->size();
    if (jar_entry->no_size_in_local_header()) {
      // The size of the data descriptor varies. The actual data in it is three
      // uint32's (crc32, compressed size, uncompressed size), but these can be
      // preceded by the "PK\x7\x8" signature word (alas, 'jar' has it).
      // Reading the descriptor just to figure out whether we need to copy four
      // or three words will cost us another page read, let us assume the data
      // description is always 4 words long at the cost of having an occasional
      // one word gap between the entries.
      num_bytes += jar_entry->compressed_file_size() + 4 * sizeof(uint32_t);
    } else {
      num_bytes += lh->compressed_file_size();
    }
    off_t output_position = Position();

    // When normalize_timestamps is set, entry's timestamp is to be set to
    // 01/01/1980 00:00:00 (or to 01/01/1980 00:00:02, if an entry is a .class
    // file). This is somewhat expensive because we have to copy the local
    // header to memory as input jar is memory mapped as read-only. Try to copy
    // as little as possible.
    uint16_t normalized_time = 0;
    const UnixTimeExtraField *lh_field_to_remove = nullptr;
    bool fix_timestamp = false;
    if (options_->normalize_timestamps) {
      if (ends_with(file_name, file_name_length, ".class")) {
        normalized_time = 1;
      }
      lh_field_to_remove = lh->unix_time_extra_field();
      fix_timestamp = jar_entry->last_mod_file_date() != 33 ||
                      jar_entry->last_mod_file_time() != normalized_time ||
                      lh_field_to_remove != nullptr;
    }
    if (fix_timestamp) {
      uint8_t lh_buffer[512];
      size_t lh_size = lh->size();
      LH *lh_new = lh_size > sizeof(lh_buffer)
                       ? reinterpret_cast<LH *>(malloc(lh_size))
                       : reinterpret_cast<LH *>(lh_buffer);
      // Remove Unix timestamp field.
      if (lh_field_to_remove != nullptr) {
        auto from_end = byte_ptr(lh) + lh->size();
        size_t removed_size = lh_field_to_remove->size();
        size_t chunk1_size = byte_ptr(lh_field_to_remove) - byte_ptr(lh);
        size_t chunk2_size = lh->size() - (chunk1_size + removed_size);
        memcpy(lh_new, lh, chunk1_size);
        if (chunk2_size) {
          memcpy(reinterpret_cast<uint8_t *>(lh_new) + chunk1_size,
                 from_end - chunk2_size, chunk2_size);
        }
        lh_new->extra_fields(lh_new->extra_fields(),
                             lh->extra_fields_length() - removed_size);
      } else {
        memcpy(lh_new, lh, lh_size);
      }
      lh_new->last_mod_file_date(33);
      lh_new->last_mod_file_time(normalized_time);
      // Now write these few bytes and adjust read/write positions accordingly.
      if (!WriteBytes(lh_new, lh_new->size())) {
        diag_err(1, "%s:%d: Cannot copy modified local header for %.*s",
                 __FILE__, __LINE__, file_name_length, file_name);
      }
      copy_from += lh_size;
      num_bytes -= lh_size;
      if (reinterpret_cast<uint8_t *>(lh_new) != lh_buffer) {
        free(lh_new);
      }
    }

    // Do the actual copy.
    ssize_t n_copied = AppendFile(input_jar.fd(), copy_from, num_bytes);
    if (n_copied < 0) {
      diag_err(1, "%s:%d: Cannot copy %ld bytes of %.*s from %s", __FILE__,
               __LINE__, num_bytes, file_name_length, file_name,
               input_jar_path.c_str());
    } else if (static_cast<size_t>(n_copied) != num_bytes) {
      diag_err(1, "%s:%d: Copied only %ld bytes out of %ld from %s", __FILE__,
               __LINE__, n_copied, num_bytes, input_jar_path.c_str());
    }

    // Append central directory header for this file to the output central
    // directory we are building.
    TODO(output_position < 0xFFFFFFFF, "Handle Zip64");

    CDH *out_cdh;
    auto field_to_remove =
        fix_timestamp ? jar_entry->unix_time_extra_field() : nullptr;
    if (field_to_remove != nullptr) {
      // Remove extra fields.
      auto from_start = byte_ptr(jar_entry);
      auto from_end = from_start + jar_entry->size();
      size_t removed_size = field_to_remove->size();
      size_t chunk1_size = byte_ptr(field_to_remove) - from_start;
      size_t chunk2_size = jar_entry->size() - (chunk1_size + removed_size);
      out_cdh =
          reinterpret_cast<CDH *>(ReserveCdr(jar_entry->size() - removed_size));
      memcpy(out_cdh, jar_entry, chunk1_size);
      if (chunk2_size) {
        memcpy(reinterpret_cast<uint8_t *>(out_cdh) + chunk1_size,
               from_end - chunk2_size, chunk2_size);
      }
      out_cdh->extra_fields(out_cdh->extra_fields(),
                            jar_entry->extra_fields_length() - removed_size);
    } else {
      out_cdh = AppendToDirectoryBuffer(jar_entry);
    }
    out_cdh->local_header_offset32(output_position);
    if (fix_timestamp) {
      out_cdh->last_mod_file_time(normalized_time);
      out_cdh->last_mod_file_date(33);
    }
    ++entries_;
  }
  return input_jar.Close();
}

off_t OutputJar::Position() {
  if (file_ == nullptr) {
    diag_err(1, "%s:%d: output file is not open", __FILE__, __LINE__);
  }
  // You'd think this could be "return ftell(file_);", but that
  // generates a needless call to lseek.  So instead we cache our
  // current position in the output.
  TODO(outpos_ < 0xFFFFFFFF, "Handle Zip64");
  return outpos_;
}

// Writes an entry. The argument is the pointer to the contiguous block of
// memory containing Local Header for the entry, immediately followed by
// the data. The memory is freed after the data has been written.
void OutputJar::WriteEntry(void *buffer) {
  if (buffer == nullptr) {
    return;
  }
  LH *entry = reinterpret_cast<LH *>(buffer);
  if (options_->verbose) {
    fprintf(stderr, "%-.*s combiner has %lu bytes, %s to %lu\n",
            entry->file_name_length(), entry->file_name(),
            entry->uncompressed_file_size(),
            entry->compression_method() == Z_NO_COMPRESSION ? "copied"
                                                            : "compressed",
            entry->compressed_file_size());
  }

  // Set this entry's timestamp.
  // MSDOS file timestamp format that Zip uses is described here:
  // https://msdn.microsoft.com/en-us/library/9kkf9tah.aspx
  // ("32-Bit Windows Time/Date Formats")
  if (options_->normalize_timestamps) {
    // Regular "normalized" timestamp is 01/01/1980 00:00:00, while for the
    // .class file it is 01/01/1980 00:00:02
    entry->last_mod_file_date(33);
    entry->last_mod_file_time(
        ends_with(entry->file_name(), entry->file_name_length(), ".class") ? 1
                                                                           : 0);
  } else {
    struct tm tm;
    // Time has 2-second resolution, so round up:
    time_t t_adjusted = (time(nullptr) + 1) & ~1;
    localtime_r(&t_adjusted, &tm);
    uint16_t dos_date =
        ((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday;
    uint16_t dos_time =
        (tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec >> 1);
    entry->last_mod_file_time(dos_time);
    entry->last_mod_file_date(dos_date);
  }

  uint8_t *data = reinterpret_cast<uint8_t *>(entry);
  off_t output_position = Position();
  if (!WriteBytes(data, entry->data() + entry->in_zip_size() - data)) {
    diag_err(1, "%s:%d: write", __FILE__, __LINE__);
  }
  // Data written, allocate CDH space and populate CDH.
  CDH *cdh = reinterpret_cast<CDH *>(
      ReserveCdh(sizeof(CDH) + entry->file_name_length()));
  cdh->signature();
  // Note: do not set the version to Unix 3.0 spec, otherwise
  // unzip will think that 'external_attributes' field contains access mode
  cdh->version(20);
  cdh->version_to_extract(20);  // 2.0
  cdh->bit_flag(0x0);
  cdh->compression_method(entry->compression_method());
  cdh->last_mod_file_time(entry->last_mod_file_time());
  cdh->last_mod_file_date(entry->last_mod_file_date());
  cdh->crc32(entry->crc32());
  TODO(entry->compressed_file_size32() != 0xFFFFFFFF, "Handle Zip64");
  cdh->compressed_file_size32(entry->compressed_file_size32());
  TODO(entry->uncompressed_file_size32() != 0xFFFFFFFF, "Handle Zip64");
  cdh->uncompressed_file_size32(entry->uncompressed_file_size32());
  cdh->file_name(entry->file_name(), entry->file_name_length());
  cdh->extra_fields(nullptr, 0);
  cdh->comment_length(0);
  cdh->start_disk_nr(0);
  cdh->internal_attributes(0);
  cdh->external_attributes(0);
  cdh->local_header_offset32(output_position);
  ++entries_;
  free(reinterpret_cast<void *>(entry));
}

void OutputJar::AddDirectory(const char *path) {
  size_t n_path = strlen(path);
  size_t lh_size = sizeof(LH) + n_path;
  LH *lh = reinterpret_cast<LH *>(malloc(lh_size));
  lh->signature();
  lh->version(20);  // 2.0
  lh->bit_flag(0);  // TODO(asmundak): should I set UTF8 flag?
  lh->compression_method(Z_NO_COMPRESSION);
  lh->crc32(0);
  lh->compressed_file_size32(0);
  lh->uncompressed_file_size32(0);
  lh->file_name(path, n_path);
  lh->extra_fields(nullptr, 0);
  known_members_.emplace(path, EntryInfo{&null_combiner_});
  WriteEntry(lh);
}

// Appends a Central Directory Entry to the directory buffer.
CDH *OutputJar::AppendToDirectoryBuffer(const CDH *cdh) {
  size_t cdh_size = cdh->size();
  return reinterpret_cast<CDH *>(
      memcpy(reinterpret_cast<CDH *>(ReserveCdr(cdh_size)), cdh, cdh_size));
}

uint8_t *OutputJar::ReserveCdr(size_t chunk_size) {
  if (cen_size_ + chunk_size > cen_capacity_) {
    cen_capacity_ += 1000000;
    cen_ = reinterpret_cast<uint8_t *>(realloc(cen_, cen_capacity_));
    if (!cen_) {
      diag_errx(1, "%s:%d: Cannot allocate %ld bytes for the directory",
                __FILE__, __LINE__, cen_capacity_);
    }
  }
  uint8_t *entry = cen_ + cen_size_;
  cen_size_ += chunk_size;
  return entry;
}

uint8_t *OutputJar::ReserveCdh(size_t size) {
  return static_cast<uint8_t *>(memset(ReserveCdr(size), 0, size));
}

// Write out combined jar.
bool OutputJar::Close() {
  if (file_ == nullptr) {
    return true;
  }

  for (auto &service_handler : service_handlers_) {
    WriteEntry(service_handler->OutputEntry(options_->force_compression));
  }
  for (auto &extra_combiner : extra_combiners_) {
    WriteEntry(extra_combiner->OutputEntry(options_->force_compression));
  }
  WriteEntry(spring_handlers_.OutputEntry(options_->force_compression));
  WriteEntry(spring_schemas_.OutputEntry(options_->force_compression));
  WriteEntry(protobuf_meta_handler_.OutputEntry(options_->force_compression));
  // TODO(asmundak): handle manifest;
  off_t output_position = Position();
  bool write_zip64_ecd = output_position >= 0xFFFFFFFF || entries_ >= 0xFFFF ||
                         cen_size_ >= 0xFFFFFFFF;

  TODO(output_position < 0xFFFFFFFF, "Handle Zip64");

  size_t cen_size = cen_size_;  // Save it before ReserveCdh updates it.
  if (write_zip64_ecd) {
    ECD64 *ecd64 = reinterpret_cast<ECD64 *>(ReserveCdh(sizeof(ECD64)));
    ECD64Locator *ecd64_locator =
        reinterpret_cast<ECD64Locator *>(ReserveCdh(sizeof(ECD64Locator)));
    ECD *ecd = reinterpret_cast<ECD *>(ReserveCdh(sizeof(ECD)));
    ecd64->signature();
    ecd64->remaining_size(sizeof(ECD64) - 12);
    ecd64->version(0x031E);         // Unix, version 3.0
    ecd64->version_to_extract(45);  // 4.5 (Zip64 support)
    ecd64->this_disk_entries(entries_);
    ecd64->total_entries(entries_);
    ecd64->cen_size(cen_size);
    ecd64->cen_offset(output_position);
    ecd64_locator->signature();
    ecd64_locator->ecd64_offset(output_position + cen_size);
    ecd64_locator->total_disks(1);
    ecd->signature();
    ecd->this_disk_entries16(0xFFFF);
    ecd->total_entries16(0xFFFF);
    // Java Compiler (javac) uses its own "optimized" Zip handler (see
    // https://bugs.openjdk.java.net/browse/JDK-7018859) which may fail
    // to handle 0xFFFFFFFF in the CEN size and CEN offset fields. Try
    // to use 32-bit values here, too. Hopefully by the time we need to
    // handle really large archives, this is fixes upstream. Note that this
    // affects javac and javah only, 'jar' experiences no problems.
    ecd->cen_size32(std::min(cen_size, static_cast<size_t>(0xFFFFFFFFUL)));
    ecd->cen_offset32(
        std::min(output_position, static_cast<off_t>(0x0FFFFFFFFL)));
  } else {
    ECD *ecd = reinterpret_cast<ECD *>(ReserveCdh(sizeof(ECD)));
    ecd->signature();
    ecd->this_disk_entries16((uint16_t)entries_);
    ecd->total_entries16((uint16_t)entries_);
    ecd->cen_size32(cen_size);
    ecd->cen_offset32(output_position);
  }

  // Save Central Directory and wrap up.
  if (!WriteBytes(cen_, cen_size_)) {
    diag_err(1, "%s:%d: Cannot write central directory", __FILE__, __LINE__);
  }
  free(cen_);

  if (fclose(file_)) {
    diag_err(1, "%s:%d: %s", __FILE__, __LINE__, path());
  }
  file_ = nullptr;
  // Free the buffer only after fclose(); stdio may flush data from the
  // buffer on close.
  buffer_.reset();

  if (options_->verbose) {
    fprintf(stderr, "Wrote %s with %d entries", path(), entries_);
    if (duplicate_entries_) {
      fprintf(stderr, ", skipped %d entries", duplicate_entries_);
    }
    fprintf(stderr, "\n");
  }
  return true;
}

void OutputJar::ClasspathResource(const std::string &resource_name,
                                  const std::string &resource_path) {
  if (known_members_.count(resource_name)) {
    if (options_->warn_duplicate_resources) {
      diag_warnx(
          "%s:%d: Duplicate resource name %s in the --classpath_resource or "
          "--resource option",
          __FILE__, __LINE__, resource_name.c_str());
      // TODO(asmundak): this mimics old behaviour. Confirm that unless
      // we run with --warn_duplicate_resources, the output zip file contains
      // the concatenated contents of the all the resources with the same name.
      return;
    }
  }
  MappedFile mapped_file;
  if (!mapped_file.Open(resource_path)) {
    diag_err(1, "%s:%d: %s", __FILE__, __LINE__, resource_path.c_str());
  }
  Concatenator *classpath_resource = new Concatenator(resource_name);
  classpath_resource->Append(
      reinterpret_cast<const char *>(mapped_file.start()), mapped_file.size());
  classpath_resources_.emplace_back(classpath_resource);
  known_members_.emplace(resource_name, EntryInfo{classpath_resource});
}

ssize_t OutputJar::AppendFile(int in_fd, off_t offset, size_t count) {
  if (count == 0) {
    return 0;
  }
  std::unique_ptr<void, decltype(free)*> buffer(malloc(kBufferSize), free);
  if (buffer == nullptr) {
    diag_err(1, "%s:%d: malloc", __FILE__, __LINE__);
  }
  ssize_t total_written = 0;

  while (total_written < count) {
    size_t len = std::min(kBufferSize, count - total_written);
    ssize_t n_read = pread(in_fd, buffer.get(), len, offset + total_written);
    if (n_read > 0) {
      if (!WriteBytes(buffer.get(), n_read)) {
        return -1;
      }
      total_written += n_read;
    } else if (n_read == 0) {
      break;
    } else {
      return -1;
    }
  }

  return total_written;
}

void OutputJar::ExtraCombiner(const std::string &entry_name,
                              Combiner *combiner) {
  extra_combiners_.emplace_back(combiner);
  known_members_.emplace(entry_name, EntryInfo{combiner});
}

bool OutputJar::WriteBytes(void *buffer, size_t count) {
  size_t written = fwrite(buffer, 1, count, file_);
  outpos_ += written;
  return written == count;
}

void OutputJar::ExtraHandler(const CDH *) {}
