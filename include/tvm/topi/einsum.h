/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file topi/einsum.h
 * \brief Einstein summation op
 */
#ifndef TVM_TOPI_EINSUM_H_
#define TVM_TOPI_EINSUM_H_

#include <tvm/te/operation.h>
#include <tvm/tir/data_layout.h>
#include <tvm/topi/detail/constant_utils.h>
#include <tvm/topi/detail/ravel_unravel.h>
#include <tvm/topi/detail/tensor_utils.h>
#include <tvm/topi/tags.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>
#include <bitset>

namespace tvm {
namespace topi {

using namespace tvm::te;
using namespace topi::detail;

inline Array<PrimExpr> get_stride(const Array<PrimExpr> shape) {
  size_t ndim = shape.size();
  int prod = 1;
  Array<PrimExpr> stride = Array<PrimExpr>(ndim, -1);
  for (int i = ndim - 1; i >= 0; i--) {
    stride.Set(i, if_then_else(shape[i] > 1, prod, 0));
    prod = prod * GetConstInt(shape[i]);
  }
  return stride;
}

inline Array<PrimExpr> pad(const Array<PrimExpr> shape, int odim) {
  int ndim = shape.size();
  CHECK_GE(odim, ndim);
  Array<PrimExpr> ret(static_cast<size_t>(odim), 1);
  for (int idim = 0; idim < ndim; ++idim) {
    ret.Set(idim, shape[idim]);
  }
  return ret;
}

inline int parse_operand_subscripts(const char *subscripts, int length,
                                    int ndim, int iop, char *op_labels,
                                    char *label_counts, int *min_label, int *max_label) {
  int i;
  int idim = 0;
  int ellipsis = -1;

  /* Process all labels for this operand */
  for (i = 0; i < length; ++i) {
    int label = subscripts[i];

    /* A proper label for an axis. */
    if (label > 0 && isalpha(label)) {
      /* Check we don't exceed the operator dimensions. */
      CHECK(idim < ndim)
        << "einstein sum subscripts string contains "
        << "too many subscripts for operand "
        << iop;

      op_labels[idim++] = label;
      if (label < *min_label) {
        *min_label = label;
      }
      if (label > *max_label) {
        *max_label = label;
      }
      label_counts[label]++;
    } else if (label == '.') {
      /* The beginning of the ellipsis. */
      /* Check it's a proper ellipsis. */
      CHECK(!(ellipsis != -1 || i + 2 >= length
              || subscripts[++i] != '.' || subscripts[++i] != '.'))
        << "einstein sum subscripts string contains a "
        << "'.' that is not part of an ellipsis ('...') "
        << "in operand "
        << iop;

      ellipsis = idim;
    } else {
        CHECK(label == ' ')
          << "invalid subscript '" << static_cast<char>(label)
          << "' in einstein sum "
          << "subscripts string, subscripts must "
          << "be letters";
    }
  }

  /* No ellipsis found, labels must match dimensions exactly. */
  if (ellipsis == -1) {
    CHECK(idim == ndim)
      << "operand has more dimensions than subscripts "
      << "given in einstein sum, but no '...' ellipsis "
      << "provided to broadcast the extra dimensions.";
  } else if (idim < ndim) {
    /* Ellipsis found, may have to add broadcast dimensions. */
    /* Move labels after ellipsis to the end. */
    for (i = 0; i < idim - ellipsis; ++i) {
      op_labels[ndim - i - 1] = op_labels[idim - i - 1];
    }
    /* Set all broadcast dimensions to zero. */
    for (i = 0; i < ndim - idim; ++i) {
      op_labels[ellipsis + i] = 0;
    }
  }

  /*
   * Find any labels duplicated for this operand, and turn them
   * into negative offsets to the axis to merge with.
   *
   * In C, the char type may be signed or unsigned, but with
   * twos complement arithmetic the char is ok either way here, and
   * later where it matters the char is cast to a signed char.
   */
  for (idim = 0; idim < ndim - 1; ++idim) {
    int label = op_labels[idim];
    /* If it is a proper label, find any duplicates of it. */
    if (label > 0) {
      /* Search for the next matching label. */
      char *next = reinterpret_cast<char*>(memchr(op_labels + idim + 1, label, ndim - idim - 1));

      while (next != nullptr) {
        /* The offset from next to op_labels[idim] (negative). */
        *next = static_cast<char>((op_labels + idim) - next);
        /* Search for the next matching label. */
        next = reinterpret_cast<char*>(memchr(next + 1, label, op_labels + ndim - 1 - next));
      }
    }
  }
  return 0;
}

inline int parse_output_subscripts(const char *subscripts, int length,
                                   int ndim_broadcast,
                                   const char *label_counts, char *out_labels) {
  int i, bdim;
  int ndim = 0;
  int ellipsis = 0;

  /* Process all the output labels. */
  for (i = 0; i < length; ++i) {
    int label = subscripts[i];

    /* A proper label for an axis. */
    if (label > 0 && isalpha(label)) {
      /* Check that it doesn't occur again. */
      CHECK(memchr(subscripts + i + 1, label, length - i - 1) == nullptr)
        << "einstein sum subscripts string includes "
        << "output subscript '" << static_cast<char>(label)
        << "' multiple times";

      /* Check that it was used in the inputs. */
      CHECK(label_counts[label] != 0)
        << "einstein sum subscripts string included "
        << "output subscript '" << static_cast<char>(label)
        << "' which never appeared "
        << "in an input";

      /* Check that there is room in out_labels for this label. */
      CHECK(ndim < 16)
        << "einstein sum subscripts string contains "
        << "too many subscripts in the output";

      out_labels[ndim++] = label;
    } else if (label == '.') {
      /* The beginning of the ellipsis. */
      /* Check it is a proper ellipsis. */
      CHECK(!(ellipsis || i + 2 >= length
              || subscripts[++i] != '.' || subscripts[++i] != '.'))
        << "einstein sum subscripts string "
        << "contains a '.' that is not part of "
        << "an ellipsis ('...') in the output";

      /* Check there is room in out_labels for broadcast dims. */
      CHECK(ndim + ndim_broadcast <= 16)
        << "einstein sum subscripts string contains "
        << "too many subscripts in the output";

      ellipsis = 1;
      for (bdim = 0; bdim < ndim_broadcast; ++bdim) {
        out_labels[ndim++] = 0;
      }
    } else {
      CHECK(label == ' ')
        << "invalid subscript '" << static_cast<char>(label)
        << "' in einstein sum "
        << "subscripts string, subscripts must "
        << "be letters";
    }
  }

  /* If no ellipsis was found there should be no broadcast dimensions. */
  CHECK(!(!ellipsis && ndim_broadcast > 0))
    << "output has more dimensions than subscripts "
    << "given in einstein sum, but no '...' ellipsis "
    << "provided to broadcast the extra dimensions.";

  return ndim;
}

inline void GetCombinedDimsView(const Tensor& op, int iop,
                                   char *labels,
                                   Array<PrimExpr> newshape,
                                   Array<PrimExpr> newstride) {
  int idim, ndim, icombine, combineoffset;
  int icombinemap[16];
  int newdim;

  Array<PrimExpr> shape = op->shape;
  Array<PrimExpr> stride = get_stride(shape);
  ndim = op.ndim();
  newdim = newshape.size();

  /* Initialize the dimensions and strides to zero */
  for (idim = 0; idim < newdim; ++idim) {
    newshape.Set(idim, 0);
    newstride.Set(idim, 0);
  }

  /* Copy the dimensions and strides, except when collapsing */
  icombine = 0;
  for (idim = 0; idim < ndim; ++idim) {
    /*
     * The char type may be either signed or unsigned, we
     * need it to be signed here.
     */
    int label = (signed char)labels[idim];
    /* If this label says to merge axes, get the actual label */
    if (label < 0) {
      combineoffset = label;
      label = labels[idim+label];
    } else {
      combineoffset = 0;
      if (icombine != idim) {
        labels[icombine] = labels[idim];
      }
      icombinemap[idim] = icombine;
    }
    /* If the label is 0, it's an unlabeled broadcast dimension */
    if (label == 0) {
      newshape.Set(icombine, shape[idim]);
      newstride.Set(icombine, stride[idim]);
    } else {
      /* Update the combined axis dimensions and strides */
      int i = icombinemap[idim + combineoffset];
      CHECK(!((combineoffset < 0) && GetConstInt(newshape[i] != 0 && newshape[i] != shape[idim])))
        << "dimensions in operand " << iop
        << " for collapsing index '" << label
        << "' don't match (" << GetConstInt(newshape[i])
        << " != " << shape[idim] << ")";
      newshape.Set(i, shape[idim]);
      newstride.Set(i, newstride[i] + stride[idim]);
    }

    /* If the label didn't say to combine axes, increment dest i */
    if (combineoffset == 0) {
      icombine++;
    }
  }
}

inline static int prepare_op_axes(int ndim, int iop, char *labels,
                                  int *axes, int ndim_iter, char *iter_labels) {
  int i, label, ibroadcast;

  ibroadcast = ndim-1;
  for (i = ndim_iter-1; i >= 0; --i) {
    label = iter_labels[i];
    /*
     * If it's an unlabeled broadcast dimension, choose
     * the next broadcast dimension from the operand.
     */
    if (label == 0) {
      while (ibroadcast >= 0 && labels[ibroadcast] != 0) {
        --ibroadcast;
      }
      /*
       * If we used up all the operand broadcast dimensions,
       * extend it with a "newaxis"
       */
      if (ibroadcast < 0) {
        axes[i] = -1;
      } else {
        /* Otherwise map to the broadcast axis */
        axes[i] = ibroadcast;
        --ibroadcast;
      }
    } else {
      /* It's a labeled dimension, find the matching one */
      char *match = reinterpret_cast<char*>(memchr(labels, label, ndim));
      /* If the op doesn't have the label, broadcast it */
      if (match == nullptr) {
        axes[i] = -1;
      } else {
        /* Otherwise use it */
        axes[i] = match - labels;
      }
    }
  }
  return 0;
}

inline int _count_substring(const std::string& str,
                            const std::string& sub) {
  int count = 0;
  std::string::size_type pos = 0;
  while ((pos = str.find(sub, pos)) != std::string::npos) {
    ++count;
    pos += sub.length();
  }
  return count;
}

inline std::bitset<128> str2set(const std::string& str) {
  std::bitset<128> ret;
  for (const char& c : str) {
    ret.set(static_cast<int>(c));
  }
  return ret;
}

inline std::vector<std::string> split(const std::string& str,
                                      const std::string& sub) {
  std::string::size_type pos = 0;
  std::string::size_type start = 0;
  std::vector<std::string> ret;
  while ((pos = str.find(sub, start)) != std::string::npos) {
    ret.push_back(str.substr(start, pos - start));
    start = pos + sub.length();
  }
  ret.push_back(str.substr(start));
  return ret;
}

inline std::vector<std::string> _parse_einsum_input(
  std::string subscripts,
  const std::vector<Array<PrimExpr>>& operands) {
  const std::string einsum_symbols =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::bitset<128> einsum_symbols_set;
  for (const char& c : einsum_symbols) {
    einsum_symbols_set.set(c);
  }

  CHECK_NE(operands.size(), 0U)
    << "No input operands";

  auto end_pos = std::remove(subscripts.begin(), subscripts.end(), ' ');
  subscripts.erase(end_pos, subscripts.end());

  // Ensure all characters are valid
  for (const char& c : subscripts) {
    if (c == '.' || c == ',' || c == '-' || c == '>') {
      continue;
    }
    CHECK(einsum_symbols_set.test(c))
      << "Character " << c
      << " is not a valid symbol.";
  }

  // Check for proper "->"
  if (subscripts.find('-') != std::string::npos ||
      subscripts.find('>') != std::string::npos) {
    bool invalid = (std::count(subscripts.begin(), subscripts.end(), '-') > 1 ||
                    std::count(subscripts.begin(), subscripts.end(), '>') > 1);
    CHECK(!invalid && _count_substring(subscripts, "->") == 1)
      << "Subscripts can only contain one '->'.";
  }

  // Parse ellipses
  if (subscripts.find('.') != std::string::npos) {
    std::string used = subscripts;
    used.erase(std::remove_if(used.begin(),
                              used.end(),
                              [](const char& c){return c == '.' ||
                                                       c == ',' ||
                                                       c == '-' ||
                                                       c == '>';}),
               used.end());

    std::bitset<128> used_set = str2set(used);
    std::string ellipse_inds = "";
    for (const char& c : einsum_symbols) {
      if (!used_set.test(static_cast<int>(c))) {
        ellipse_inds.append(1, c);
      }
    }
    int longest = 0;
    std::string input_tmp, output_sub;
    std::vector<std::string> split_subscripts;
    bool out_sub;

    if (subscripts.find("->") != std::string::npos) {
      std::vector<std::string> tmp = split(subscripts, "->");
      input_tmp = tmp[0];
      output_sub = tmp[1];
      split_subscripts = split(input_tmp, ",");
      out_sub = true;
    } else {
      split_subscripts = split(subscripts, ",");
      out_sub = false;
    }

    size_t size_split_subscripts = split_subscripts.size();
    subscripts = "";
    for (size_t i = 0; i < size_split_subscripts; ++i) {
      const std::string& sub = split_subscripts[i];
      if (sub.find('.') != std::string::npos) {
        CHECK_EQ(std::count(sub.begin(), sub.end(), '.'), 3)
          << "Invalid Ellipses";
        CHECK_EQ(_count_substring(sub, "..."), 1)
          << "Invalid Ellipses";

        // Take into account numerical values
        int ellipse_count = 0;
        if (operands[i].size() == 0) {
          ellipse_count = 0;
        } else {
          ellipse_count = std::max(operands[i].size(), static_cast<size_t>(1));
          ellipse_count -= sub.length() - 3;
        }

        if (ellipse_count > longest) {
          longest = ellipse_count;
        }

        CHECK_GE(ellipse_count, 0)
          << "Ellipses lengths do not match.";
        if (ellipse_count == 0) {
          split_subscripts[i].erase(sub.find("..."), 3);
        } else {
          std::string rep_inds = ellipse_inds.substr(ellipse_inds.length() - ellipse_count);
          split_subscripts[i].replace(sub.find("..."), 3, rep_inds);
        }
      }
      subscripts += split_subscripts[i];
      if (i + 1 < size_split_subscripts) {
        subscripts += ",";
      }
    }
    std::string out_ellipse;
    if (longest == 0) {
      out_ellipse = "";
    } else {
      out_ellipse = ellipse_inds.substr(ellipse_inds.length() - longest);
    }

    if (out_sub) {
      output_sub.replace(output_sub.find("..."), 3, out_ellipse);
      subscripts += "->" + output_sub;
    } else {
      // Special care for outputless ellipses
      std::bitset<128> out_ellipse_set = str2set(out_ellipse);
      std::string tmp_subscripts = subscripts, output_subscript = "";
      size_t len_tmp_subscripts = tmp_subscripts.length();
      std::sort(tmp_subscripts.begin(), tmp_subscripts.end());
      for (size_t i = 0; i < len_tmp_subscripts; ++i) {
        const char& c = tmp_subscripts[i];
        if (c == ',') {
          continue;
        }
        CHECK(einsum_symbols_set.test(c))
          << "Character " << c
          << " is not a valid symbol.";
        if ((i == 0 || tmp_subscripts[i - 1] != c) &&
            (i == len_tmp_subscripts - 1 || tmp_subscripts[i + 1] != c) &&
            !out_ellipse_set.test(c)) {
          output_subscript.append(1, c);
        }
      }
      subscripts += "->" + out_ellipse + output_subscript;
    }
  }

  // Build output string if does not exist
  std::vector<std::string> ret(2);
  if (subscripts.find("->") != std::string::npos) {
    ret = split(subscripts, "->");
  } else {
    ret[0] = subscripts;
    ret[1] = "";
    // Build output subscripts
    std::string tmp_subscripts = subscripts;
    size_t len_tmp_subscripts = tmp_subscripts.length();
    std::sort(tmp_subscripts.begin(), tmp_subscripts.end());
    for (size_t i = 0; i < len_tmp_subscripts; ++i) {
      const char& c = tmp_subscripts[i];
      if (c == ',') {
        continue;
      }
      CHECK(einsum_symbols_set.test(c))
        << "Character " << c
        << " is not a valid symbol.";
      if ((i == 0 || tmp_subscripts[i - 1] != c) &&
          (i == len_tmp_subscripts - 1 || tmp_subscripts[i + 1] != c)) {
        ret[1].append(1, c);
      }
    }
  }

  // Make sure output subscripts are in the input
  std::bitset<128> input_subscripts_set = str2set(ret[0]);
  for (const char& c : ret[1]) {
    CHECK(input_subscripts_set.test(c))
      << "Output character " << c
      << " did not appear in the input";
  }

  // Make sure number operands is equivalent to the number of terms
  CHECK_EQ(std::count(ret[0].begin(), ret[0].end(), ',') + 1, operands.size())
    << "Number of einsum subscripts must be equal to the "
    << "number of operands.";

  return ret;
}

inline Array<PrimExpr> NumpyEinsumShape(const std::string subscripts,
                                        const std::vector<Array<PrimExpr>>& operands) {
  // Parsing
  std::vector<std::string> parsed_subscripts = _parse_einsum_input(subscripts, operands);

  // Build a few useful list and sets
  std::vector<std::string> input_list = split(parsed_subscripts[0], ",");
  size_t isize = input_list.size();

  // Get length of each unique dimension and ensure all dimensions are correct
  int dimension_dict[128];
  memset(dimension_dict, -1, sizeof(dimension_dict));
  for (size_t i = 0; i < isize; ++i) {
    const std::string& term = input_list[i];
    const Array<PrimExpr>& sh = operands[i];
    CHECK_EQ(sh.size(), term.length())
      << "Einstein sum subscript " << input_list[i]
      << " does not contain the "
      << "correct number of indices for operand " << i << ".";
    size_t len_term = term.length();
    for (size_t j = 0; j < len_term; ++j) {
      int64_t dim = GetConstInt(sh[j]);
      const char& c = term[j];

      if (dimension_dict[static_cast<int>(c)] != -1) {
        // For broadcasting cases we always want the largest dim size
        if (dimension_dict[static_cast<int>(c)] == 1) {
          dimension_dict[static_cast<int>(c)] = dim;
        }
        CHECK(dim == 1 || dim == dimension_dict[static_cast<int>(c)])
          << "Size of label '" << c
          << "' for operand  " << i
          << " (" << dimension_dict[static_cast<int>(c)]
          << ") does not match previous terms ("
          << dim << ").";
      } else {
        dimension_dict[static_cast<int>(c)] = dim;
      }
    }
  }

  // Get oshape
  const std::string& output_str = parsed_subscripts[1];
  size_t odim = output_str.size();
  Array<PrimExpr> oshape(odim, -1);
  for (size_t i = 0; i < odim; ++i) {
    oshape.Set(i, dimension_dict[static_cast<int>(output_str[i])]);
  }
  // Neglecting oshape assign check temporally
  return oshape;
}

/*!
 * \brief Evaluates the Einstein summation convention on the operands.
 *
 * \param subscripts Specifies the subscripts for summation as comma separated list of subscript labels.
 * \param inputs Arrays for the operation
 * \param name The name of the operation
 * \param tag The tag to mark the operation
 *
 * \return The calculation based on the Einstein summation convention.
 */
inline Tensor einsum(const std::string& subscripts_str, const Array<Tensor> inputs,
                     std::string name = "T_einsum", std::string tag = kMatMul) {
  // able to compute op: trace, diag, sum, transpose, matmul, dot, inner, outer, multiply, tensordot
  bool back = false;
  const char* subscripts = subscripts_str.data();
  const int nop = inputs.size();

  /* Step 1: Parse the subscripts string into label_counts and op_labels */
  int iop, idim, min_label = 127, max_label = 0;
  char label_counts[128], op_labels[16][16];
  memset(label_counts, 0, sizeof(label_counts));
  for (iop = 0; iop < nop; ++iop) {
    int length = static_cast<int>(strcspn(subscripts, ",-"));

    CHECK(!(iop == nop - 1 && subscripts[length] == ','))
      << "more operands provided to einstein sum function "
      << "than specified in the subscripts string";
    CHECK(!(iop < nop-1 && subscripts[length] != ','))
      << "fewer operands provided to einstein sum function "
      << "than specified in the subscripts string";
    parse_operand_subscripts(subscripts, length,
                                      inputs[iop + back].ndim(),
                                      iop, op_labels[iop], label_counts,
                                      &min_label, &max_label);

    /* Move subscripts to the start of the labels for the next op */
    subscripts += length;
    if (iop < nop - 1) {
      subscripts++;
    }
  }
  /*
   * Find the number of broadcast dimensions, which is the maximum
   * number of labels == 0 in an op_labels array.
   */
  int ndim_broadcast = 0;
  for (iop = 0; iop < nop; ++iop) {
    int count_zeros = 0;
    int ndim;
    char *labels = op_labels[iop];

    ndim = inputs[iop + back].ndim();
    for (idim = 0; idim < ndim; ++idim) {
      if (labels[idim] == 0) {
        ++count_zeros;
      }
    }

    if (count_zeros > ndim_broadcast) {
      ndim_broadcast = count_zeros;
    }
  }

  /*
   * If there is no output signature, fill output_labels and ndim_output
   * using each label that appeared once, in alphabetical order.
   */
  int label, ndim_output;
  char output_labels[16];
  if (subscripts[0] == '\0') {
    /* If no output was specified, always broadcast left, as usual. */
    for (ndim_output = 0; ndim_output < ndim_broadcast; ++ndim_output) {
      output_labels[ndim_output] = 0;
    }
    for (label = min_label; label <= max_label; ++label) {
      if (label_counts[label] == 1) {
        CHECK(ndim_output < 16)
          << "einstein sum subscript string has too many "
          << "distinct labels";
        output_labels[ndim_output++] = label;
      }
    }
  } else {
    CHECK(subscripts[0] == '-' && subscripts[1] == '>')
      << "einstein sum subscript string does not "
      << "contain proper '->' output specified";
    subscripts += 2;

    /* Parse the output subscript string. */
    ndim_output = parse_output_subscripts(subscripts, strlen(subscripts),
                                          ndim_broadcast, label_counts,
                                          output_labels);
    CHECK_GE(ndim_output, 0);
  }

  /*
   * Step 2:
   * Process all the input ops, combining dimensions into their
   * diagonal where specified.
   */
  std::vector<Array<PrimExpr>> opshape(nop), opstride_true(nop);
  for (iop = 0; iop < nop; ++iop) {
    char *labels = op_labels[iop];
    int combine, ndim;

    ndim = inputs[iop + back].ndim();

    /*
     * Check whether any dimensions need to be combined
     *
     * The char type may be either signed or unsigned, we
     * need it to be signed here.
     */
    combine = 0;
    for (idim = 0; idim < ndim; ++idim) {
      if ((signed char)labels[idim] < 0) {
        combine++;
      }
    }
    /* If any dimensions are combined, create a view which combines them */
    if (combine) {
      Array<PrimExpr> tshape(static_cast<size_t>(ndim - combine), -1);
      Array<PrimExpr> tstride(static_cast<size_t>(ndim - combine), -1);
      GetCombinedDimsView(inputs[iop + back], iop, labels,
                             tshape, tstride);
      opshape[iop] = tshape;
      opstride_true[iop] = tstride;
    } else {
      /* No combining needed */
      opshape[iop] = inputs[iop + back]->shape;
      opstride_true[iop] = get_stride(opshape[iop]);
    }
  }
  /*
   * Step 3:
   * Set up the labels for the iterator (output + combined labels).
   * Can just share the output_labels memory, because iter_labels
   * is output_labels with some more labels appended.
   */
  char *iter_labels = output_labels;
  int ndim_iter = ndim_output;
  for (label = min_label; label <= max_label; ++label) {
    if (label_counts[label] > 0 &&
        memchr(output_labels, label, ndim_output) == nullptr) {
      CHECK(ndim_iter < 16)
        << "too many subscripts in einsum";
      iter_labels[ndim_iter++] = label;
    }
  }
  /* Step 4: Set up the op_axes for the iterator */
  Array<PrimExpr> itershape(static_cast<size_t>(ndim_iter), -1);
  std::vector<Array<PrimExpr>> iterstride(nop + 1,
                                Array<PrimExpr>(static_cast<size_t>(ndim_iter), 0));

  // output_shape
  std::vector<Array<PrimExpr>> operands;
  for (size_t i = 0; i < inputs.size(); i++) {
    operands.push_back(inputs[i]->shape);
  }
  Array<PrimExpr> oshape = NumpyEinsumShape(subscripts_str, operands);
  Array<PrimExpr> ostride_true = get_stride(oshape);
  Array<PrimExpr> reduceshape;
  std::vector<Array<PrimExpr>> remainshape(nop);
  int op_axes_arrays[16][16];
  int *op_axes[16];
  for (iop = 0; iop < nop; ++iop) {
    op_axes[iop] = op_axes_arrays[iop];
    CHECK_GE(prepare_op_axes(opshape[iop].size(), iop, op_labels[iop],
             op_axes[iop], ndim_iter, iter_labels), 0);
    for (idim = 0; idim < ndim_iter; idim++) {
      if (op_axes[iop][idim] != -1) {
        iterstride[iop].Set(idim, opstride_true[iop][op_axes[iop][idim]]);
        if (GetConstInt(itershape[idim]) != -1) {
          if (GetConstInt(itershape[idim]) == 1) {
            itershape.Set(idim, opshape[iop][op_axes[iop][idim]]);
          }
        } else {
          itershape.Set(idim, opshape[iop][op_axes[iop][idim]]);
        }
      }
    }
  }
  for (idim = 0; idim < ndim_output; ++idim) {
    iterstride[nop].Set(idim, ostride_true[idim]);
  }
  reduceshape = Array<PrimExpr>(static_cast<size_t>(ndim_iter - ndim_output), 0);
  for (idim = ndim_output; idim < ndim_iter; ++idim) {
    reduceshape.Set(idim - ndim_output, itershape[idim]);
  }
  for (iop = 0; iop < nop; iop++) {
    Array<Integer> rsh;
    for (idim = 0; idim < ndim_iter; idim++) {
      if (op_axes_arrays[iop][idim] == -1) {
        rsh.push_back(GetConstInt(itershape[idim]));
      } else {
        if (GetConstInt(itershape[idim] != opshape[iop][op_axes_arrays[iop][idim]])) {
          rsh.push_back(GetConstInt(itershape[idim]));
        }
      }
    }
    remainshape[iop] = Array<PrimExpr>(rsh.begin(), rsh.end());
  }
  // exclude the 0-dim case
  if (ndim_iter == 0) {
    ndim_iter = 1;
  }
  itershape = pad(itershape, ndim_iter);
  for (iop = 0; iop <= nop; ++iop) {
    iterstride[iop] = pad(iterstride[iop], ndim_iter);
  }
  // oshape = pad(oshape, ndim_iter);
  reduceshape = pad(reduceshape, ndim_iter);
  for (iop = 0; iop < nop; ++iop) {
    opshape[iop] = pad(opshape[iop], ndim_iter);
    remainshape[iop] = pad(remainshape[iop], ndim_iter);
  }
  // ostride and rstride
  Array<Array<PrimExpr>> ostride;
  Array<Array<PrimExpr>> rstride;

  for (iop = 0; iop < nop; ++iop) {
    Array<PrimExpr> otmp(static_cast<size_t>(ndim_iter), 0);
    Array<PrimExpr> rtmp(static_cast<size_t>(ndim_iter), 0);
    for (idim = 0; idim < ndim_iter; ++idim) {
      otmp.Set(idim, idim < ndim_output ? iterstride[iop][idim] : 1);
      rtmp.Set(idim, idim < ndim_iter - ndim_output ? iterstride[iop][idim+ndim_output] : 1);
    }
    ostride.push_back(otmp);
    rstride.push_back(rtmp);
  }


  // func: input indices => return cooresponding value
  auto func = [inputs, oshape, ostride, reduceshape, ndim_iter,
               rstride, nop](const Array<Var>& input_indices) -> PrimExpr {
    for (int rdim = 0; rdim < ndim_iter; ++rdim) {
      if (GetConstInt(reduceshape[rdim]) == 0) {
        return 0;  //
      }
    }
    Array<PrimExpr> ridx = UnravelIndex(0, reduceshape);
    // AType needs to fit the output data
    PrimExpr sum = 0;
    bool rec_flag = false;
    do {
      PrimExpr tmp = 1;
      for (int iop = 0; iop < nop; ++iop) {
        if (iop != -1) {
            PrimExpr k = 0;
            for (size_t i = 0; i < input_indices.size(); ++i) {
              k += input_indices[i] * ostride[iop][i];
            }
            for (size_t i = 0; i < ridx.size(); ++i) {
              k += ridx[i] * rstride[iop][i];
            }
           Array<PrimExpr> temp_indices = UnravelIndex(k, inputs[iop]->shape);
           tmp = tmp * inputs[iop](temp_indices);
        }
      }
      sum += tmp;
      ridx.Set(ridx.size() - 1, ridx[ridx.size()-1] + 1);
      for (int i = static_cast<int>(ridx.size() - 1); (i > 0) &&
                                  GetConstInt(ridx[i] >= reduceshape[i]); --i) {
        ridx.Set(i, ridx[i] - reduceshape[i]);
        ridx.Set(i-1, ridx[i-1] + 1);
      }
      rec_flag = GetConstInt(ridx[0] < reduceshape[0]);
    } while (rec_flag);
    return sum;
  };

  return compute(oshape, func, name, tag);
}

}  // namespace topi
}  // namespace tvm
#endif  // TVM_TOPI_EINSUM_H_
