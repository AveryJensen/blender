/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2020 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup obj
 */

#include <fstream>
#include <iostream>

#include "BKE_context.h"

#include "BLI_map.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "wavefront_obj_ex_file_writer.hh"
#include "wavefront_obj_im_file_reader.hh"

namespace blender::io::obj {

using std::string;

/**
 * Split the given string by the delimiter and fill the given vector.
 * If an intermediate string is empty, or space or null character, it is not appended to the
 * vector.
 */
static void split_by_char(const string &in_string, char delimiter, Vector<string> &r_out_list)
{
  std::stringstream stream(in_string);
  string word{};
  while (std::getline(stream, word, delimiter)) {
    if (word.empty() || word[0] == ' ' || word[0] == '\0') {
      continue;
    }
    r_out_list.append(word);
  }
}

/**
 * Split a line string into the first word (key) and the rest of the line with the
 * first space in the latter removed.
 */
static void split_line_key_rest(std::string_view line, string &r_line_key, string &r_rest_line)
{
  if (line.empty()) {
    return;
  }
  size_t pos = line.find_first_of(' ');
  r_line_key = pos == string::npos ? line.substr(0, 1) : line.substr(0, pos);
  r_rest_line = line.substr(r_line_key.size(), string::npos);
  if (r_rest_line.empty()) {
    return;
  }
  /* Remove the space between line key and the data after it. */
  r_rest_line.erase(0, 1);
}

/**
 * Convert the given string to float and assign it to the destination value.
 *
 * Catches exception if the string cannot be converted to a float. The destination value
 * is set to the given fallback value in that case.
 */

void copy_string_to_float(StringRef src, const float fallback_value, float &r_dst)
{
  try {
    r_dst = std::stof(src.data());
  }
  catch (const std::invalid_argument &inv_arg) {
    fprintf(stderr, "Bad conversion to float:%s:%s\n", inv_arg.what(), src.data());
    r_dst = fallback_value;
  }
}

/**
 * Convert all members of the Span of strings to floats and assign them to the float
 * array members. Usually used for values like coordinates.
 *
 * Catches exception if any string cannot be converted to a float. The destination
 * float is set to the given fallback value in that case.
 */

BLI_INLINE void copy_string_to_float(Span<string> src,
                                     const float fallback_value,
                                     MutableSpan<float> r_dst)
{
  BLI_assert(src.size() == r_dst.size());
  for (int i = 0; i < r_dst.size(); ++i) {
    copy_string_to_float(src[i], fallback_value, r_dst[i]);
  }
}

/**
 * Convert the given string to int and assign it to the destination value.
 *
 * Catches exception if the string cannot be converted to an integer. The destination
 * int is set to the given fallback value in that case.
 */
BLI_INLINE void copy_string_to_int(StringRef src, const int fallback_value, int &r_dst)
{
  try {
    r_dst = std::stoi(src.data());
  }
  catch (const std::invalid_argument &inv_arg) {
    fprintf(stderr, "Bad conversion to int:%s:%s\n", inv_arg.what(), src.data());
    r_dst = fallback_value;
  }
}

/**
 * Convert the given strings to ints and fill the destination int buffer.
 *
 * Catches exception if any string cannot be converted to an integer. The destination
 * int is set to the given fallback value in that case.
 */
BLI_INLINE void copy_string_to_int(Span<string> src,
                                   const int fallback_value,
                                   MutableSpan<int> r_dst)
{
  BLI_assert(src.size() == r_dst.size());
  for (int i = 0; i < r_dst.size(); ++i) {
    copy_string_to_int(src[i], fallback_value, r_dst[i]);
  }
}

/**
 * Based on the properties of the given Geometry instance, return whether a new Geometry instance
 * should be created. Caller should get some hint that the encountered object is a curve before
 * calling this function.
 *
 * This relies on the fact that the object type is updated to include CU_NURBS only _after_
 * this function returns true.
 */
static bool create_geometry_curve(Geometry *geometry)
{
  if (geometry) {
    /* After the creation of a Geometry instance, at least one element has been found in the OBJ
     * file that indicates that it is a mesh, not a curve. */
    if (geometry->tot_face_elems() || geometry->tot_uv_verts() || geometry->tot_normals()) {
      return true;
    }
    /* If not, then the given object could be a curve with all fields complete.
     * So create a new Geometry only if its type doesn't contain GEOM_CURVE. */
    return geometry->geom_type() & GEOM_CURVE;
  }
  return true;
}

/**
 * Open OBJ file at the path given in import parameters.
 */
OBJParser::OBJParser(const OBJImportParams &import_params) : import_params_(import_params)
{
  obj_file_.open(import_params_.filepath);
}

/**
 * Always update these offsets whenever a new object is created.
 * See the documentation of index offsets member array too.
 */
void OBJParser::update_index_offsets(Geometry *geometry)
{
  if (geometry) {
    if (geometry->geom_type_ & GEOM_MESH) {
      index_offsets_[VERTEX_OFF] += geometry->vertex_indices_.size();
      index_offsets_[UV_VERTEX_OFF] += geometry->uv_vertex_indices_.size();
    }
    else if (geometry->geom_type_ & GEOM_CURVE) {
      index_offsets_[VERTEX_OFF] += geometry->nurbs_element_.curv_indices.size();
    }
  }
}

/**
 * Read the OBJ file line by line and create OBJ Geometry instances. Also store all the vertex
 * and UV vertex coordinates in a struct accessible by all objects.
 */
void OBJParser::parse_and_store(Vector<std::unique_ptr<Geometry>> &all_geometries,
                                GlobalVertices &global_vertices)
{
  if (!obj_file_.good()) {
    fprintf(stderr, "Cannot read from file:%s.\n", import_params_.filepath);
    return;
  }

  string line;
  /* Non owning raw pointer to a Geometry.
   * Needed to update object data in the same while loop. */
  Geometry *current_geometry = nullptr;
  /* State-setting variables: if set, they remain the same for the remaining
   * elements in the object. */
  bool shaded_smooth = false;
  string object_group{};

  while (std::getline(obj_file_, line)) {
    string line_key{}, rest_line{};
    split_line_key_rest(line, line_key, rest_line);
    if (line.empty() || rest_line.empty()) {
      continue;
    }

    if (line_key == "mtllib") {
      mtl_libraries_.append(rest_line);
    }
    else if (line_key == "o") {
      /* Update index offsets to keep track of objects which have claimed their vertices. */
      update_index_offsets(current_geometry);
      shaded_smooth = false;
      object_group = {};
      all_geometries.append(std::make_unique<Geometry>(GEOM_MESH, rest_line));
      current_geometry = all_geometries.last().get();
    }
    else if (line_key == "v") {
      float3 curr_vert{};
      Vector<string> str_vert_split;
      split_by_char(rest_line, ' ', str_vert_split);
      copy_string_to_float(str_vert_split, FLT_MAX, {curr_vert, 3});
      global_vertices.vertices.append(curr_vert);
      if (current_geometry) {
        /* Always keep indices zero-based. */
        current_geometry->vertex_indices_.append(global_vertices.vertices.size() - 1);
      }
    }
    else if (line_key == "vn") {
      current_geometry->tot_normals_++;
    }
    else if (line_key == "vt") {
      float2 curr_uv_vert{};
      Vector<string> str_uv_vert_split;
      split_by_char(rest_line, ' ', str_uv_vert_split);
      copy_string_to_float(str_uv_vert_split, FLT_MAX, {curr_uv_vert, 2});
      global_vertices.uv_vertices.append(curr_uv_vert);
      if (current_geometry) {
        current_geometry->uv_vertex_indices_.append(global_vertices.uv_vertices.size() - 1);
      }
    }
    else if (line_key == "l") {
      BLI_assert(current_geometry);
      int edge_v1 = -1, edge_v2 = -1;
      Vector<string> str_edge_split;
      split_by_char(rest_line, ' ', str_edge_split);
      copy_string_to_int(str_edge_split[0], -1, edge_v1);
      copy_string_to_int(str_edge_split[1], -1, edge_v2);
      /* Remove the indices of vertices "claimed" by other Geometry instances. Subtract 1 to make
       * the OBJ indices (one-based) C++'s zero-based. In the other case, make relative index
       * positive and absolute, starting with zero. */
      edge_v1 -= edge_v1 > 0 ? index_offsets_[VERTEX_OFF] + 1 : -(global_vertices.vertices.size());
      edge_v2 -= edge_v2 > 0 ? index_offsets_[VERTEX_OFF] + 1 : -(global_vertices.vertices.size());
      BLI_assert(edge_v1 >= 0 && edge_v2 >= 0);
      current_geometry->edges_.append({static_cast<uint>(edge_v1), static_cast<uint>(edge_v2)});
    }
    else if (line_key == "g") {
      if (!current_geometry) {
        all_geometries.append(std::make_unique<Geometry>(GEOM_MESH, rest_line));
        current_geometry = all_geometries.last().get();
      }
      object_group = rest_line;
      if (object_group.find("off") != string::npos || object_group.find("null") != string::npos) {
        /* Set group for future elements like faces or curves to empty. */
        object_group = {};
      }
    }
    else if (line_key == "s") {
      /* Some implementations use "0" and "null" too, in addition to "off". */
      if (rest_line != "0" && rest_line.find("off") == string::npos &&
          rest_line.find("null") == string::npos) {
        /* TODO ankitm make a string to bool function if need arises. */
        try {
          std::stoi(rest_line);
          shaded_smooth = true;
        }
        catch (const std::invalid_argument &inv_arg) {
          fprintf(stderr,
                  "Bad argument for smooth shading: %s:%s\n",
                  inv_arg.what(),
                  rest_line.c_str());
          shaded_smooth = false;
        }
      }
      else {
        /* The OBJ file explicitly set shading to off. */
        shaded_smooth = false;
      }
    }
    else if (line_key == "f") {
      BLI_assert(current_geometry);
      FaceElement curr_face;
      curr_face.shaded_smooth = shaded_smooth;
      if (!object_group.empty()) {
        curr_face.vertex_group = object_group;
        /* Yes it repeats several times, but another if-check will not reduce steps either. */
        current_geometry->use_vertex_groups_ = true;
      }

      Vector<string> str_corners_split;
      split_by_char(rest_line, ' ', str_corners_split);
      for (const string &str_corner : str_corners_split) {
        FaceCorner corner;
        size_t n_slash = std::count(str_corner.begin(), str_corner.end(), '/');
        if (n_slash == 0) {
          /* Case: f v1 v2 v3 . */
          copy_string_to_int(str_corner, INT32_MAX, corner.vert_index);
        }
        else if (n_slash == 1) {
          /* Case: f v1/vt1 v2/vt2 v3/vt3 . */
          Vector<string> vert_uv_split;
          split_by_char(str_corner, '/', vert_uv_split);
          copy_string_to_int(vert_uv_split[0], INT32_MAX, corner.vert_index);
          if (vert_uv_split.size() == 2) {
            copy_string_to_int(vert_uv_split[1], INT32_MAX, corner.uv_vert_index);
            current_geometry->tot_uv_verts_++;
          }
        }
        else if (n_slash == 2) {
          /* Case: f v1//vn1 v2//vn2 v3//vn3 . */
          /* Case: f v1/vt1/vn1 v2/vt2/vn2 v3/vt3/vn3 . */
          Vector<string> vert_uv_normal_split;
          split_by_char(str_corner, '/', vert_uv_normal_split);
          copy_string_to_int(vert_uv_normal_split[0], INT32_MAX, corner.vert_index);
          if (vert_uv_normal_split.size() == 3) {
            copy_string_to_int(vert_uv_normal_split[1], INT32_MAX, corner.uv_vert_index);
            current_geometry->tot_uv_verts_++;
          }
          /* Discard normals. They'll be calculated on the basis of smooth
           * shading flag. */
        }
        corner.vert_index += corner.vert_index < 0 ? index_offsets_[VERTEX_OFF] + 1 :
                                                     -(index_offsets_[VERTEX_OFF] + 1);
        corner.uv_vert_index += corner.uv_vert_index < 0 ? index_offsets_[UV_VERTEX_OFF] + 1 :
                                                           -(index_offsets_[UV_VERTEX_OFF] + 1);

        curr_face.face_corners.append(corner);
      }

      current_geometry->face_elements_.append(curr_face);
      current_geometry->tot_loops_ += curr_face.face_corners.size();
    }
    else if (line_key == "cstype") {
      if (rest_line.find("bspline") != string::npos) {
        if (create_geometry_curve(current_geometry)) {
          update_index_offsets(current_geometry);
          all_geometries.append(std::make_unique<Geometry>(GEOM_CURVE, "NURBSCurve"));
          current_geometry = all_geometries.last().get();
          current_geometry->nurbs_element_.group_ = object_group;
        }
      }
      else {
        fprintf(stderr, "Type:'%s' is not supported\n", rest_line.c_str());
      }
    }
    else if (line_key == "deg") {
      copy_string_to_int(rest_line, 3, current_geometry->nurbs_element_.degree);
    }
    else if (line_key == "curv") {
      Vector<string> str_curv_split;
      split_by_char(rest_line, ' ', str_curv_split);
      /* Remove "0.0" and "1.0" from the strings. They are hardcoded. */
      str_curv_split.remove(0);
      str_curv_split.remove(0);
      current_geometry->nurbs_element_.curv_indices.resize(str_curv_split.size());
      copy_string_to_int(str_curv_split, INT32_MAX, current_geometry->nurbs_element_.curv_indices);
      for (int &curv_index : current_geometry->nurbs_element_.curv_indices) {
        curv_index -= curv_index > 0 ? 1 : -(global_vertices.vertices.size());
      }
    }
    else if (line_key == "parm") {
      Vector<string> str_parm_split;
      split_by_char(rest_line, ' ', str_parm_split);
      if (str_parm_split[0] == "u" || str_parm_split[0] == "v") {
        str_parm_split.remove(0);
        current_geometry->nurbs_element_.parm.resize(str_parm_split.size());
        copy_string_to_float(str_parm_split, FLT_MAX, current_geometry->nurbs_element_.parm);
      }
      else {
        fprintf(stderr, "Surfaces are not supported: %s\n", str_parm_split[0].c_str());
      }
    }
    else if (line_key == "end") {
      /* Curves mark their end this way. */
      object_group = {};
    }
    else if (line_key == "usemtl") {
      current_geometry->material_name_.append(rest_line);
    }
  }
}

/**
 * Return a list of all material library filepaths referenced by the OBJ file.
 */
Span<std::string> OBJParser::mtl_libraries() const
{
  return mtl_libraries_;
}

/**
 * Open material library file.
 */
MTLParser::MTLParser(StringRef mtl_library, StringRef obj_filepath) : mtl_library_(mtl_library)
{
  char obj_file_dir[FILE_MAXDIR];
  BLI_split_dir_part(obj_filepath.data(), obj_file_dir, FILE_MAXDIR);
  BLI_path_join(mtl_file_path_, FILE_MAX, obj_file_dir, mtl_library_.data(), NULL);
  mtl_file_.open(mtl_file_path_);
}

/**
 * Read MTL file(s) and add MTLMaterial instances to the given Map reference.
 */
void MTLParser::parse_and_store(Map<string, MTLMaterial> &mtl_materials)
{
  if (!mtl_file_.good()) {
    fprintf(stderr, "Cannot read from file:%s\n", mtl_file_path_);
  }

  string line;
  MTLMaterial *current_mtlmaterial = nullptr;
  while (std::getline(mtl_file_, line)) {
    string line_key{}, rest_line{};
    split_line_key_rest(line, line_key, rest_line);
    if (line.empty() || rest_line.empty()) {
      continue;
    }

    if (line_key == "newmtl") {
      current_mtlmaterial = &mtl_materials.lookup_or_add_default_as(rest_line);
    }
    else if (line_key == "Ns") {
      copy_string_to_float(rest_line, 324.0f, current_mtlmaterial->Ns);
    }
    else if (line_key == "Ka") {
      Vector<string> str_ka_split{};
      split_by_char(rest_line, ' ', str_ka_split);
      copy_string_to_float(str_ka_split, 0.0f, {current_mtlmaterial->Ka, 3});
    }
    else if (line_key == "Kd") {
      Vector<string> str_kd_split{};
      split_by_char(rest_line, ' ', str_kd_split);
      copy_string_to_float(str_kd_split, 0.8f, {current_mtlmaterial->Kd, 3});
    }
    else if (line_key == "Ks") {
      Vector<string> str_ks_split{};
      split_by_char(rest_line, ' ', str_ks_split);
      copy_string_to_float(str_ks_split, 0.5f, {current_mtlmaterial->Ks, 3});
    }
    else if (line_key == "Ke") {
      Vector<string> str_ke_split{};
      split_by_char(rest_line, ' ', str_ke_split);
      copy_string_to_float(str_ke_split, 0.0f, {current_mtlmaterial->Ke, 3});
    }
    else if (line_key == "Ni") {
      copy_string_to_float(rest_line, 1.45f, current_mtlmaterial->Ni);
    }
    else if (line_key == "d") {
      copy_string_to_float(rest_line, 1.0f, current_mtlmaterial->d);
    }
    else if (line_key == "illum") {
      copy_string_to_int(rest_line, 2, current_mtlmaterial->illum);
    }
    /* Image Textures. */
    else if (line_key.find("map_") != string::npos) {
      if (!current_mtlmaterial->texture_maps.contains_as(line_key)) {
        /* No supported texture map found. */
        continue;
      }
      tex_map_XX &tex_map = current_mtlmaterial->texture_maps.lookup(line_key);
      Vector<string> str_map_xx_split{};
      split_by_char(rest_line, ' ', str_map_xx_split);

      int64_t pos_o{str_map_xx_split.first_index_of_try("-o")};
      if (pos_o != string::npos && pos_o + 3 < str_map_xx_split.size()) {
        copy_string_to_float({str_map_xx_split[pos_o + 1],
                              str_map_xx_split[pos_o + 2],
                              str_map_xx_split[pos_o + 3]},
                             0.0f,
                             {tex_map.translation, 3});
      }
      int64_t pos_s{str_map_xx_split.first_index_of_try("-s")};
      if (pos_s != string::npos && pos_s + 3 < str_map_xx_split.size()) {
        copy_string_to_float({str_map_xx_split[pos_s + 1],
                              str_map_xx_split[pos_s + 2],
                              str_map_xx_split[pos_s + 3]},
                             1.0f,
                             {tex_map.scale, 3});
      }
      /* Only specific to Normal Map node. */
      int64_t pos_bm{str_map_xx_split.first_index_of_try("-bm")};
      if (pos_bm != string::npos && pos_bm + 1 < str_map_xx_split.size()) {
        copy_string_to_float(
            str_map_xx_split[pos_bm + 1], 0.0f, current_mtlmaterial->map_Bump_strength);
      }

      tex_map.image_path = str_map_xx_split.last();
    }
  }
}
}  // namespace blender::io::obj
