/// \file FileIO/Fileutils.h
///
/// An abstract base class referring to an image on disk.
///
#ifndef __VW_FILEIO_FILEUTILS_H__
#define __VW_FILEIO_FILEUTILS_H__

#include <string>
#include <boost/filesystem/path.hpp>

namespace vw{

  boost::filesystem::path make_file_relative_to_dir(boost::filesystem::path const file,
                                                    boost::filesystem::path const dir);

  /// Remove file name extension
  std::string prefix_from_filename(std::string const& filename);

  /// Return lower-case extension, such as .cub (so the dot is also present).
  std::string get_extension(std::string const& input, bool make_lower = true);

  /// If prefix is "dir/out", create directory "dir"
  void create_out_dir(std::string out_prefix);

} // namespace vw

#endif // __VW_FILEIO_FILEUTILS_H__
