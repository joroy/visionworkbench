
#include <queue>
#include <vw/Stereo/SGM.h>
#include <vw/Stereo/SGMAssist.h>
#include <vw/Core/Debugging.h>
#include <vw/Core/ThreadPool.h>
#include <vw/Image/MaskViews.h>
#include <vw/Image/PixelMask.h>
#include <vw/Cartography/GeoReferenceUtils.h>

#if defined(VW_ENABLE_SSE) && (VW_ENABLE_SSE==1)
  #include <emmintrin.h>
  #include <smmintrin.h> // SSE4.1
#endif

namespace vw {

namespace stereo {



//=========================================================================

void SemiGlobalMatcher::set_parameters(CostFunctionType cost_type,
                                       bool use_mgm,
                                       int min_disp_x, int min_disp_y,
                                       int max_disp_x, int max_disp_y,
                                       int kernel_size, 
                                       SgmSubpixelMode subpixel,
                                       Vector2i search_buffer,
                                       uint16 p1, uint16 p2,
                                       int ternary_census_threshold) {
  m_cost_type   = cost_type;
  m_use_mgm     = use_mgm;
  m_min_disp_x  = min_disp_x;
  m_min_disp_y  = min_disp_y;
  m_max_disp_x  = max_disp_x;
  m_max_disp_y  = max_disp_y;
  m_kernel_size = kernel_size;
  m_ternary_census_threshold = ternary_census_threshold;
  m_subpixel_type = subpixel;
  m_search_buffer = search_buffer;
  
  m_num_disp_x = m_max_disp_x - m_min_disp_x + 1;
  m_num_disp_y = m_max_disp_y - m_min_disp_y + 1;
  size_t size_check = m_num_disp_x * m_num_disp_y;;
  if (size_check > (size_t)std::numeric_limits<DisparityType>::max())
    vw_throw( NoImplErr() << "Number of disparities is too large for data type!\n" );
  m_num_disp   = m_num_disp_x * m_num_disp_y;   

  if (p1 > 0) // User provided
    m_p1 = p1; 
  else { // Choose based on the kernel size and cost type
    switch(cost_type) {
    case CENSUS_TRANSFORM:
      switch(kernel_size){
      case 3:  m_p1 = 3;  break; // 0 - 8
      case 5:  m_p1 = 15; break; // 0 - 24
      case 7:  m_p1 = 30; break; // 0 - 48
      default: m_p1 = 3; break; // Unsupported!
      };
      break;
    case TERNARY_CENSUS_TRANSFORM:
      switch(kernel_size){
      case 5:  m_p1 = 30; break; // 0 - 48
      case 7:  m_p1 = 40; break; // 0 - 64
      case 9:  m_p1 = 40; break; // 0 - 64
      default: m_p1 = 30; break; // Unsupported!
      };
      break;
    default: // MAD
      m_p1 = 3; // 0 - 255 range
      break;
    };
  } // End p1 cases
  
  if (p2 > 0) // User provided
    m_p2 = p2;
  else { 
    switch(cost_type) {
    case CENSUS_TRANSFORM:
      switch(kernel_size){
      case 3:  m_p2 = 70;   break; // 0 - 8
      case 5:  m_p2 = 750;  break; // 0 - 24
      case 7:  m_p2 = 1500; break; // 0 - 48
      default: m_p2 = 22;   break; // Unsupported!
      };
      break;
    case TERNARY_CENSUS_TRANSFORM:
      switch(kernel_size){
      case 5:  m_p2 = 1500; break; // 0 - 48
      case 7:  m_p2 = 2000; break; // 0 - 64
      case 9:  m_p2 = 2000; break; // 0 - 64
      default: m_p2 = 30;   break; // Unsupported!
      };
      break;
    default: // MAD
      m_p2 = 250; // 0 - 255 range
      break;
    };
  } // End p2 cases

  vw_out(InfoMessage, "stereo") << "SGM m_p1 = " << m_p1 << std::endl;
  vw_out(InfoMessage, "stereo") << "SGM m_p2 = " << m_p2 << std::endl;

  
} // End set_parameters


void SemiGlobalMatcher::populate_constant_disp_bound_image() {
  // Allocate the image
  m_disp_bound_image.set_size(m_num_output_cols, m_num_output_rows);
  // Fill it up with an identical vector
  Vector4i bounds_vector(m_min_disp_x, m_min_disp_y, m_max_disp_x, m_max_disp_y);
  size_t buffer_size = m_num_output_cols*m_num_output_rows;
  //std::cout << "Init disparity image to size " << m_num_output_cols << ", " << m_num_output_rows << std::endl;
  std::fill(m_disp_bound_image.data(), m_disp_bound_image.data()+buffer_size, bounds_vector);
}

bool SemiGlobalMatcher::populate_disp_bound_image(ImageView<uint8> const* left_image_mask,
                                                  ImageView<uint8> const* right_image_mask,
                                                  DisparityImage const* prev_disparity) {

  //Timer timer_total("Populate disparity bounds");

  vw_out(VerboseDebugMessage, "stereo") << "disparity bound image size = " << bounding_box(m_disp_bound_image) << std::endl;

  // The masks are assumed to be the same size as the output image.
  // TODO: Check or automatically compute the left valid mask size!
  bool left_mask_valid = false, right_mask_valid = false;
  if (left_image_mask) {
    vw_out(VerboseDebugMessage, "stereo") << "Left  mask image size:" << bounding_box(*left_image_mask ) << std::endl;
    // Currently the left mask is required to EXACTLY match the output size...
    if ( (left_image_mask->cols() == m_disp_bound_image.cols()) && 
         (left_image_mask->rows() == m_disp_bound_image.rows())   )
      left_mask_valid = true;
    else
      vw_throw( LogicErr() << "Left mask size does not match the output size!\n" );
  }
  if (right_image_mask) {
    vw_out(VerboseDebugMessage, "stereo") << "Right mask image size:" << bounding_box(*right_image_mask) << std::endl;
    
    // Currently this class assumes a positive search range and requires the right mask to
    //  be big enough to contain the output size PLUS the search range.
    if ( (right_image_mask->cols() >= m_disp_bound_image.cols()+m_num_disp_x-1) && 
         (right_image_mask->rows() >= m_disp_bound_image.rows()+m_num_disp_y-1)   )
      right_mask_valid = true;
    else
      vw_throw( LogicErr() << "Right mask size is not large enough to support search range!\n" );
  }

  // The low-res disparity image must be half-resolution.
  const int SCALE_UP = 2; 
  
  // Require that the right image mask is valid for this percentage of the
  // search range for each pixel in the left image.
  // - If this value is too low, many border pixels will be assigned the full
  //   search range and significantly slow down SGM!
  const double MIN_MASK_OVERLAP = 0.95;

  // There needs to be some "room" in the disparity search space for
  // us to discard prior results on the edge as not trustworthy predictors.
  // In other words, don't mark any pixels as edge if the one dimension has search space 1!
  const bool check_x_edge = ((m_max_disp_x - m_min_disp_x) > 1);
  const bool check_y_edge = ((m_max_disp_y - m_min_disp_y) > 1);

  double area = 0, percent_trusted = 0, percent_masked = 0;

  // This class will check the right image mask in an efficient manner.
  // - This implementation relies on >= 0 disparity ranges!
  IterativeMaskBoxCounter right_mask_checker(right_image_mask, Vector2i(m_num_disp_x, m_num_disp_y));

  const Vector4i ZERO_SEARCH_AREA(0, 0, -1, -1);

  ImageView<uint8> full_search_image(m_disp_bound_image.cols(), m_disp_bound_image.rows());

  // Loop through the output disparity image and compute a search range for each pixel
  int r_in, c_in;
  int dx_scaled, dy_scaled;
  PixelMask<Vector2i> input_disp;
  Vector4i bounds;
  for (int r=0; r<m_disp_bound_image.rows(); ++r) {
    r_in = r / SCALE_UP;
    for (int c=0; c<m_disp_bound_image.cols(); ++c) {

      // TODO: This will fail if not used with positive search ranges!!!!!!!!!!!!!!!!!!!!!!!
      // Verify that there is sufficient overlap with the right image mask
      if (right_mask_valid) {
        double right_percent  = right_mask_checker.next_pixel();
        bool   right_check_ok = (right_percent >= MIN_MASK_OVERLAP);
        // If none of the right mask pixels were valid, flag this pixel as invalid.
        if (!right_check_ok) {
          m_disp_bound_image(c,r) = ZERO_SEARCH_AREA;
          ++percent_masked;
          continue;
        }
      } // End right mask handling

      // If the left mask is invalid here, flag the pixel as invalid.
      // - Do this second so that our right image pixel tracker stays up to date.
      if (left_mask_valid && (left_image_mask->operator()(c,r) == 0)) {
        m_disp_bound_image(c,r) = ZERO_SEARCH_AREA;
        ++percent_masked;
        continue;
      }

      // If a previous disparity was provided, see if we have a valid disparity 
      // estimate for this pixel.
      bool good_disparity = false;
      c_in = c / SCALE_UP; // Pixel location in prior disparity map if it exists
      
      if (prev_disparity) {
        
        // Verify that the pixel we want exists
        if ( (c_in >= prev_disparity->cols()) || (r_in >= prev_disparity->rows()) ) {
        /*
          std::cout << "c    = " << c    << std::endl;
          std::cout << "r    = " << r    << std::endl;
          std::cout << "c_in = " << c_in << std::endl;
          std::cout << "r_in = " << r_in << std::endl;
          std::cout << "prev_disparity->cols() = " << prev_disparity->cols() << std::endl;
          std::cout << "prev_disparity->rows() = " << prev_disparity->rows() << std::endl;
          vw_throw( LogicErr() << "populate_disp_bound_image: Size error!\n" );
          */
        }
        else {
          input_disp = prev_disparity->operator()(c_in,r_in);
          
          // Disparity values on the edge of our 2D search range are not considered trustworthy!
          dx_scaled = input_disp[0] * SCALE_UP; 
          dy_scaled = input_disp[1] * SCALE_UP;
          
          bool on_edge = (  ( check_x_edge && ((dx_scaled <= m_min_disp_x) || (dx_scaled >= m_max_disp_x)) )
                         || ( check_y_edge && ((dy_scaled <= m_min_disp_y) || (dy_scaled >= m_max_disp_y)) ) );

          good_disparity = (is_valid(input_disp) && !on_edge);
        }
        
      } // End prev disparity check
      
      if (good_disparity) {
      
        // We are more confident in the prior disparity, search nearby.
        bounds[0]  = dx_scaled - m_search_buffer[0]; // Min x
        bounds[2]  = dx_scaled + m_search_buffer[0]; // Max X
        bounds[1]  = dy_scaled - m_search_buffer[1]; // Min y
        bounds[3]  = dy_scaled + m_search_buffer[1]; // Max y
      
        // Constrain to global limits
        if (bounds[0] < m_min_disp_x) bounds[0] = m_min_disp_x;
        if (bounds[1] < m_min_disp_y) bounds[1] = m_min_disp_y;
        if (bounds[2] > m_max_disp_x) bounds[2] = m_max_disp_x;
        if (bounds[3] > m_max_disp_y) bounds[3] = m_max_disp_y;
      
        percent_trusted += 1.0;
      } else {
        // Not a trusted prior disparity, search the entire range!
        // - This takes a long time.
        bounds = Vector4i(m_min_disp_x, m_min_disp_y, m_max_disp_x, m_max_disp_y); // DEBUG       
        full_search_image(c,r) = 255;
      }
      
      m_disp_bound_image(c,r) = bounds;
      int this_area = (bounds[3]-bounds[1]+1)*(bounds[2]-bounds[0]+1);
      area += this_area;
    } // End col loop
    right_mask_checker.advance_row();
  } // End row loop
 

  const BBox2i max_range_bbox(Vector2i(m_min_disp_x, m_min_disp_y), Vector2i(m_max_disp_x, m_max_disp_y));
  const double max_search_area = max_range_bbox.area();
  
  // Shrink the search range of full range pixels based on neighbors
  
  const int NEARBY_DISP_SEARCH_RANGE = 5; // Look this many pixels in each direction
  const int NEARBY_DISP_EXPANSION    = 3; // Grow search range from what nearby pixels have
  double percent_shrunk = 0, shrunk_area = area;
  if (prev_disparity) { // No point doing this if a previous disparity image was not provided
  
    // DEBUG
    //std::stringstream s;
    //s << m_disp_bound_image.cols();
    //write_image("full_search_image"+s.str()+".tif", full_search_image);
  
    for (int r=0; r<m_disp_bound_image.rows(); ++r) {
      // Get vertical search range
      int min_search_r = r - NEARBY_DISP_SEARCH_RANGE;
      int max_search_r = r + NEARBY_DISP_SEARCH_RANGE;
      if (min_search_r <  0                        ) min_search_r = 0;
      if (max_search_r >= m_disp_bound_image.rows()) max_search_r = m_disp_bound_image.rows()-1;
      
      for (int c=0; c<m_disp_bound_image.cols(); ++c) {
        // Skip pixels without a full search range
        if (!full_search_image(c,r))
          continue;

        // Get horizontal search range
        int min_search_c = c - NEARBY_DISP_SEARCH_RANGE;
        int max_search_c = c + NEARBY_DISP_SEARCH_RANGE;
        if (min_search_c <  0                        ) min_search_c = 0;
        if (max_search_c >= m_disp_bound_image.cols()) max_search_c = m_disp_bound_image.cols()-1;
        
        // Look through the search range
        BBox2i new_range;
        for (int rs=min_search_r; rs<=max_search_r; ++rs) {
          for (int cs=min_search_c; cs<=max_search_c; ++cs) {
            if (full_search_image(cs,rs)) // Don't look at other uncertain pixels
              continue;
            // Expand bounding box
            Vector4i vec = m_disp_bound_image(cs,rs);
            new_range.grow(Vector2i(vec[0], vec[1]));
            new_range.grow(Vector2i(vec[2], vec[3]));
          }
        }
        // Grow the bounding box a bit and then record it
        if (new_range.empty())
          continue;       
        new_range.expand(NEARBY_DISP_EXPANSION);
        new_range.crop(max_range_bbox); // Constrain to global limits
        m_disp_bound_image(c,r) = Vector4i(new_range.min().x(),   new_range.min().y(),
                                           new_range.max().x(),   new_range.max().y());
        percent_shrunk += 1.0;
        shrunk_area -= (max_search_area - new_range.area());
      }
    }
    //std::cout << "max_range_bbox = " << max_range_bbox << std::endl;
  } // End of search range shrinking code
  
  // Compute some statistics for help improving the speed
  double num_pixels = m_disp_bound_image.rows()*m_disp_bound_image.cols();
  area            /= num_pixels;
  shrunk_area     /= num_pixels;
  percent_trusted /= num_pixels;
  percent_masked  /= num_pixels;
  percent_shrunk  /= num_pixels;
  
  
  double initial_percent_full_range = 1.0 - (percent_trusted+percent_masked);
  double final_percent_full_range   = initial_percent_full_range - percent_shrunk;
  vw_out(InfoMessage, "stereo") << "Max pixel search area  = "            << max_search_area    << std::endl;
  vw_out(InfoMessage, "stereo") << "Mean pixel search area (initial) = "  << area               << std::endl;
  vw_out(InfoMessage, "stereo") << "Mean pixel search area (final  ) = "  << shrunk_area        << std::endl;
  vw_out(InfoMessage, "stereo") << "Percent trusted prior disparities = " << percent_trusted    << std::endl;
  vw_out(InfoMessage, "stereo") << "Percent masked pixels  = "            << percent_masked     << std::endl;
  vw_out(InfoMessage, "stereo") << "Percent shrunk pixels  = "            << percent_shrunk     << std::endl;
  vw_out(InfoMessage, "stereo") << "Percent full search range pixels (initial) = " << initial_percent_full_range << std::endl;
  vw_out(InfoMessage, "stereo") << "Percent full search range pixels (final  ) = " << final_percent_full_range   << std::endl;
  
  // Return false if the image cannot be processed
  if ((area <= 0) || (percent_masked >= 100))
    return false;
  return true;

} // End populate_disp_bound_image


void SemiGlobalMatcher::allocate_large_buffers() {

  //Timer timer_total("Memory allocation");

  // Init the starts data storage
  m_buffer_starts.set_size(m_num_output_cols, m_num_output_rows);

  vw_out(DebugMessage, "stereo") << "SGM: Num pixels = "      << m_num_output_rows * m_num_output_cols << std::endl;
  vw_out(DebugMessage, "stereo") << "SGM: Num disparities = " << m_num_disp << std::endl;
  
  // For each pixel, record the starting offset and add the disparity search area 
  //  of this pixel to the running offset total.
  size_t total_offset = 0;
  for (int r=0; r<m_num_output_rows; ++r) {
    for (int c=0; c<m_num_output_cols; ++c) {   
      m_buffer_starts(c,r) = total_offset;
      total_offset += get_num_disparities(c, r);
    }
  }
  // Finished computing the pixel offsets.

  vw_out(DebugMessage, "stereo") << "SGM: Total disparity search area = " << total_offset << std::endl;

  // TODO: Check available memory first!

  const size_t cost_buffer_num_bytes = total_offset * sizeof(CostType);  
  vw_out(DebugMessage, "stereo") << "SGM: Allocating buffer of size: " << cost_buffer_num_bytes/(1024*1024) << " MB\n";

  m_cost_buffer.reset(new CostType[total_offset]);

  const size_t accum_buffer_num_bytes = total_offset * sizeof(AccumCostType);  
  vw_out(DebugMessage, "stereo") << "SGM: Allocating buffer of size: " << accum_buffer_num_bytes/(1024*1024) << " MB\n";

  // Allocate the requested memory and init all to zero
  m_accum_buffer.reset(new AccumCostType[total_offset]);
  memset(m_accum_buffer.get(), 0, accum_buffer_num_bytes);
}



void SemiGlobalMatcher::populate_adjacent_disp_lookup_table() {

  const int TABLE_WIDTH = 8;
  m_adjacent_disp_lookup.resize(m_num_disp*TABLE_WIDTH);
  
  // Loop through the disparities
  int d = 0;
  for (int dy=m_min_disp_y; dy<=m_max_disp_y; ++dy) {
    // Figure out above and below disparities with bounds checking
    int y_less = dy - 1;
    int y_more = dy + 1;
    if (y_less < m_min_disp_y) y_less = dy;
    if (y_more > m_max_disp_y) y_more = dy;
    
    int y_less_o = y_less - m_min_disp_y; // Offset from min y disparity
    int y_o      = dy     - m_min_disp_y;
    int y_more_o = y_more - m_min_disp_y;
    
    for (int dx=m_min_disp_x; dx<=m_max_disp_x; ++dx) {

      // Figure out left and right disparities with bounds checking
      int x_less = dx - 1;
      int x_more = dx + 1;
      if (x_less < m_min_disp_x) x_less = dx;
      if (x_more > m_max_disp_x) x_more = dx;

      int x_less_o = x_less - m_min_disp_x; // Offset from min x disparity
      int x_o      = dx     - m_min_disp_x;
      int x_more_o = x_more - m_min_disp_x;
      
      // Record the disparity indices of each of the adjacent pixels
      int table_pos = d*TABLE_WIDTH;
      m_adjacent_disp_lookup[table_pos+0] = y_less_o*m_num_disp_x + x_o;      // The four adjacent pixels
      m_adjacent_disp_lookup[table_pos+1] = y_o     *m_num_disp_x + x_less_o;
      m_adjacent_disp_lookup[table_pos+2] = y_o     *m_num_disp_x + x_more_o;
      m_adjacent_disp_lookup[table_pos+3] = y_more_o*m_num_disp_x + x_o;
      m_adjacent_disp_lookup[table_pos+4] = y_less_o*m_num_disp_x + x_less_o; // The four diagonal pixels
      m_adjacent_disp_lookup[table_pos+5] = y_less_o*m_num_disp_x + x_more_o;
      m_adjacent_disp_lookup[table_pos+6] = y_more_o*m_num_disp_x + x_less_o;
      m_adjacent_disp_lookup[table_pos+7] = y_more_o*m_num_disp_x + x_more_o;
      
      ++d;
    }
  }
} // End function populate_adjacent_disp_lookup_table


#if not defined(VW_ENABLE_SSE) || (VW_ENABLE_SSE==0)
// Note: local and output are the same size.
// full_prior_buffer is always length m_num_disps and comes in initialized to a
//  large flag value.  When the function quits the buffer must be returned to this state.
void SemiGlobalMatcher::evaluate_path( int col, int row, int col_p, int row_p,
                       AccumCostType* const prior,
                       AccumCostType*       full_prior_buffer,
                       CostType     * const local,
                       AccumCostType*       output,
                       int path_intensity_gradient, bool debug ) {

  // Decrease p2 (jump cost) with increasing disparity along the path
  AccumCostType p2_mod = m_p2;
  if (path_intensity_gradient > 0)
    p2_mod /= path_intensity_gradient;
  if (p2_mod < m_p1)
    p2_mod = m_p1;

  //int num_disparities   = get_num_disparities(col,   row  ); // Can be input arg
  //int num_disparities_p = get_num_disparities(col_p, row_p);

  Vector4i pixel_disp_bounds   = m_disp_bound_image(col, row);
  Vector4i pixel_disp_bounds_p = m_disp_bound_image(col_p, row_p);

  // Init the min prior in case the previous pixel is invalid.
  AccumCostType BAD_VAL = get_bad_accum_val();
  AccumCostType min_prior = BAD_VAL;

  // Insert the valid disparity scores into full_prior buffer so they are
  //  easy to access quickly within the pixel loop below.
  int d = 0;
  for (int dy=pixel_disp_bounds_p[1]; dy<=pixel_disp_bounds_p[3]; ++dy) {

    // Get initial fill linear storage index for this dy row
    int full_index = xy_to_disp(pixel_disp_bounds_p[0], dy);

    for (int dx=pixel_disp_bounds_p[0]; dx<=pixel_disp_bounds_p[2]; ++dx) {
    
      // Get the min prior while we are at it.
      if (prior[d] < min_prior) {
        min_prior  = prior[d];
      }
    
      full_prior_buffer[full_index] = prior[d];
      ++full_index;
      ++d;
    }
  }
  AccumCostType min_prev_disparity_cost = min_prior + p2_mod;
  if (debug) {
    std::cout << "Prior pixel = ("<<col_p<<","<<row_p<<")\n";
    std::cout << "m_p2  : " << m_p2 << std::endl;
    std::cout << "path_intensity_gradient  : " << path_intensity_gradient << std::endl;
    std::cout << "p2_mod  : " << p2_mod << std::endl;
    std::cout << "Bounds  : " << pixel_disp_bounds << std::endl;
    std::cout << "Bounds_P: " << pixel_disp_bounds_p << std::endl;
  
    std::cout << "min_prior = " <<  min_prior << std::endl;
    std::cout << "min_prev_disparity_cost = " <<  min_prev_disparity_cost << std::endl;
    
    std::cout << "Priors: \n";
    int i=0;    
    for (int dy=pixel_disp_bounds_p[1]; dy<=pixel_disp_bounds_p[3]; ++dy) {
      for (int dx=pixel_disp_bounds_p[0]; dx<=pixel_disp_bounds_p[2]; ++dx) {
        std::cout << prior[i] << " ";
        ++i;
      }
      std::cout << std::endl;
    }
    std::cout << std::endl;

    std::cout << "Costs: \n";
    i=0;    
    for (int dy=pixel_disp_bounds[1]; dy<=pixel_disp_bounds[3]; ++dy) {
      for (int dx=pixel_disp_bounds[0]; dx<=pixel_disp_bounds[2]; ++dx) {
        std::cout << int(local[i]) << " ";
        ++i;
      }
      std::cout << std::endl;
    }
    std::cout << std::endl;
    
    std::cout << "Full prior buffer: \n";
    i = 0;
    for (int r=0; r<m_num_disp_y; ++r) {
      for (int c=0; c<m_num_disp_x; ++c) {
        std::cout << full_prior_buffer[i] << " ";
        ++i;
      }
      std::cout << std::endl;
    }
    std::cout << std::endl;
  }
  const int LOOKUP_TABLE_WIDTH = 8;
  
  // Loop through disparities for this pixel
  int packed_d = 0; // Index for cost and output vectors
  for (int dy=pixel_disp_bounds[1]; dy<=pixel_disp_bounds[3]; ++dy) {

    // Need the disparity index from all of m_num_disp for proper indexing into full_prior_buffer
    int full_d = xy_to_disp(pixel_disp_bounds[0], dy);

    for (int dx=pixel_disp_bounds[0]; dx<=pixel_disp_bounds[2]; ++dx) {

      // Start with the cost for the same disparity in the previous pixel
      AccumCostType lowest_combined_cost = full_prior_buffer[full_d];

      // Compare to the eight adjacent disparities using the lookup table
      const int lookup_index = full_d*LOOKUP_TABLE_WIDTH;

      // TODO: This is the slowest part of the algorithm!
      // Note that the lookup table indexes into a full size buffer of disparities, not the compressed
      //  buffers that are stored for each pixel.  This allows us to use a single lookup table for every pixel
      //  and avoid any bounds checking logic inside this loop.
      AccumCostType lowest_adjacent_cost =                  full_prior_buffer[m_adjacent_disp_lookup[lookup_index  ]];
      lowest_adjacent_cost = std::min(lowest_adjacent_cost, full_prior_buffer[m_adjacent_disp_lookup[lookup_index+1]]);
      lowest_adjacent_cost = std::min(lowest_adjacent_cost, full_prior_buffer[m_adjacent_disp_lookup[lookup_index+2]]);
      lowest_adjacent_cost = std::min(lowest_adjacent_cost, full_prior_buffer[m_adjacent_disp_lookup[lookup_index+3]]);
      lowest_adjacent_cost = std::min(lowest_adjacent_cost, full_prior_buffer[m_adjacent_disp_lookup[lookup_index+4]]);
      lowest_adjacent_cost = std::min(lowest_adjacent_cost, full_prior_buffer[m_adjacent_disp_lookup[lookup_index+5]]);
      lowest_adjacent_cost = std::min(lowest_adjacent_cost, full_prior_buffer[m_adjacent_disp_lookup[lookup_index+6]]);
      lowest_adjacent_cost = std::min(lowest_adjacent_cost, full_prior_buffer[m_adjacent_disp_lookup[lookup_index+7]]);

      // Now add the adjacent penalty cost and compare to the local cost
      lowest_adjacent_cost += m_p1;
      lowest_combined_cost = std::min(lowest_combined_cost, lowest_adjacent_cost);
      
      // Compare to the lowest prev disparity cost regardless of location
      lowest_combined_cost = std::min(lowest_combined_cost, min_prev_disparity_cost);
           
      // The output cost = local cost + lowest combined cost - min_prior
      // - Subtracting out min_prior avoids overflow.
      output[packed_d] = local[packed_d] + lowest_combined_cost - min_prior;

      //if (debug) {
      //  printf("Details %d: local = %d, lowest_adjacent_cost = %d, prev_cost = %d, lowest_combined_cost = %d, output = %d\n", 
      //        packed_d, local[packed_d], lowest_adjacent_cost, full_prior_buffer[full_d], lowest_combined_cost, output[packed_d]);
      //}


      ++packed_d;
      ++full_d;
    }
  } // End loop through this disparity  
  
  if(debug) {
    int min_val   = 99999;
    int min_index = 0;
    
    std::cout << "Output: \n";
    int i=0;    
    for (int dy=pixel_disp_bounds[1]; dy<=pixel_disp_bounds[3]; ++dy) {
      for (int dx=pixel_disp_bounds[0]; dx<=pixel_disp_bounds[2]; ++dx) {
        std::cout << output[i] << " ";
        if (output[i] < min_val) {
          min_val   = output[i];
          min_index = i;
        }
        ++i;
      }
      std::cout << std::endl;
    }
    std::cout << std::endl;
    
    DisparityType dx, dy;
    disp_to_xy(min_index, dx, dy);
    std::cout << "Min value = " << min_val << std::endl;
    std::cout << "Disp = " << dx-4 <<", " << dy-4 << std::endl;
    std::cout << "Min index = " << min_index << std::endl<< std::endl;
    std::cout << "========================================\n\n";
  }
  // Remove the valid disparity scores from full_prior buffer.
  for (int dy=pixel_disp_bounds_p[1]; dy<=pixel_disp_bounds_p[3]; ++dy) {

    // Get initial fill linear storage index for this dy row
    int full_index = xy_to_disp(pixel_disp_bounds_p[0], dy);

    for (int dx=pixel_disp_bounds_p[0]; dx<=pixel_disp_bounds_p[2]; ++dx) {
    
      full_prior_buffer[full_index] = BAD_VAL;
      ++full_index;
    }
  }

}
#endif

#if defined(VW_ENABLE_SSE) && (VW_ENABLE_SSE==1)
void SemiGlobalMatcher::evaluate_path( int col, int row, int col_p, int row_p,
                       AccumCostType* const prior,
                       AccumCostType*       full_prior_buffer,
                       CostType     * const local,
                       AccumCostType*       output,
                       int path_intensity_gradient, bool debug ) {

  // Decrease p2 (jump cost) with increasing disparity along the path
  AccumCostType p2_mod = m_p2;
  if (path_intensity_gradient > 0)
    p2_mod /= path_intensity_gradient;
  if (p2_mod < m_p1)
    p2_mod = m_p1;

  Vector4i pixel_disp_bounds   = m_disp_bound_image(col, row);
  Vector4i pixel_disp_bounds_p = m_disp_bound_image(col_p, row_p);

  // Init the min prior in case the previous pixel is invalid.
  AccumCostType BAD_VAL = get_bad_accum_val();
  AccumCostType min_prior = BAD_VAL;

  // Insert the valid disparity scores into full_prior buffer so they are
  //  easy to access quickly within the pixel loop below.
  // - If we don't use a full sized buffer, our adjacent disparity lookup
  //   table could not be used!
  int d = 0;
  for (int dy=pixel_disp_bounds_p[1]; dy<=pixel_disp_bounds_p[3]; ++dy) {

    // Get initial fill linear storage index for this dy row
    int full_index = xy_to_disp(pixel_disp_bounds_p[0], dy);

    for (int dx=pixel_disp_bounds_p[0]; dx<=pixel_disp_bounds_p[2]; ++dx) {
    
      // Get the min prior while we are at it.
      if (prior[d] < min_prior) {
        min_prior  = prior[d];
      }
    
      full_prior_buffer[full_index] = prior[d];
      ++full_index;
      ++d;
    }
  }
  AccumCostType min_prev_disparity_cost = min_prior + p2_mod;

  const int LOOKUP_TABLE_WIDTH = 8;
  
  // Allocate linear storage for data to pass to SSE instructions
  const int SSE_BUFF_LEN = 8;
  uint16 d_packed[SSE_BUFF_LEN*11] __attribute__ ((aligned (16))); // TODO: Could be passed in!
  uint16* dL   = &(d_packed[0*SSE_BUFF_LEN]);
  uint16* d0   = &(d_packed[1*SSE_BUFF_LEN]);
  uint16* d1   = &(d_packed[2*SSE_BUFF_LEN]);
  uint16* d2   = &(d_packed[3*SSE_BUFF_LEN]);
  uint16* d3   = &(d_packed[4*SSE_BUFF_LEN]);
  uint16* d4   = &(d_packed[5*SSE_BUFF_LEN]);
  uint16* d5   = &(d_packed[6*SSE_BUFF_LEN]);
  uint16* d6   = &(d_packed[7*SSE_BUFF_LEN]);
  uint16* d7   = &(d_packed[8*SSE_BUFF_LEN]);
  uint16* d8   = &(d_packed[9*SSE_BUFF_LEN]);
  uint16* dRes = &(d_packed[10*SSE_BUFF_LEN]); // The results
  
  // Set up constant SSE registers that never change
  __m128i _dJ  = _mm_set1_epi16(static_cast<int16>(min_prev_disparity_cost));
  __m128i _dP  = _mm_set1_epi16(static_cast<int16>(min_prior));
  __m128i _dp1 = _mm_set1_epi16(static_cast<int16>(m_p1));
  
  //printf("dJ = %d, dP = %d, dp1 = %d\n", min_prev_disparity_cost, min_prior, m_p1);
  //std::cout << "pixel_disp_bounds = " << pixel_disp_bounds << std::endl;
  
  // Loop through disparities for this pixel
  int sse_index = 0, output_index = 0;
  int packed_d = 0; // Index for cost and output vectors
  for (int dy=pixel_disp_bounds[1]; dy<=pixel_disp_bounds[3]; ++dy) {

    // Need the disparity index from all of m_num_disp for proper indexing into full_prior_buffer
    int full_d = xy_to_disp(pixel_disp_bounds[0], dy);

    for (int dx=pixel_disp_bounds[0]; dx<=pixel_disp_bounds[2]; ++dx) {

      // Get local value and matching disparity value
      dL[sse_index] = local[packed_d];
      d0[sse_index] = full_prior_buffer[full_d];
      
      // Get the 8 surrounding values.
      // - Is there any way to speed this up?
      const int lookup_index = full_d*LOOKUP_TABLE_WIDTH;
      d1[sse_index] = full_prior_buffer[m_adjacent_disp_lookup[lookup_index  ]];
      d2[sse_index] = full_prior_buffer[m_adjacent_disp_lookup[lookup_index+1]];
      d3[sse_index] = full_prior_buffer[m_adjacent_disp_lookup[lookup_index+2]];
      d4[sse_index] = full_prior_buffer[m_adjacent_disp_lookup[lookup_index+3]];
      d5[sse_index] = full_prior_buffer[m_adjacent_disp_lookup[lookup_index+4]];
      d6[sse_index] = full_prior_buffer[m_adjacent_disp_lookup[lookup_index+5]];
      d7[sse_index] = full_prior_buffer[m_adjacent_disp_lookup[lookup_index+6]];
      d8[sse_index] = full_prior_buffer[m_adjacent_disp_lookup[lookup_index+7]];

      ++packed_d;
      ++full_d;
      ++sse_index;
      
      // Keep packing the SSE buffers until they are filled up, then use SSE to operate on
      // all of the data at once.
      if (sse_index == SSE_BUFF_LEN){

        compute_path_internals_sse(dL, d0, d1, d2, d3, d4, d5, d6, d7, d8,
                                   _dJ, _dP, _dp1, dRes, sse_index, output_index, output);
        //compute_path_internals(dL, d0, d1, d2, d3, d4, d5, d6, d7, d8,
        //                   min_prev_disparity_cost, min_prior, m_p1, dRes, sse_index, output_index, output);
      
        sse_index = 0;
      } // End SSE operations
      
    }
  } // End loop through this disparity
  
  // If there is data left over in the buffer, process it now.
  if (sse_index > 0) {
    compute_path_internals_sse(dL, d0, d1, d2, d3, d4, d5, d6, d7, d8,
                           _dJ, _dP, _dp1, dRes, sse_index, output_index, output);
    //compute_path_internals(dL, d0, d1, d2, d3, d4, d5, d6, d7, d8,
    //                       min_prev_disparity_cost, min_prior, m_p1, dRes, sse_index, output_index, output);
  }
  
  // Remove the valid disparity scores from full_prior buffer.
  for (int dy=pixel_disp_bounds_p[1]; dy<=pixel_disp_bounds_p[3]; ++dy) {

    // Get initial fill linear storage index for this dy row
    int full_index = xy_to_disp(pixel_disp_bounds_p[0], dy);

    for (int dx=pixel_disp_bounds_p[0]; dx<=pixel_disp_bounds_p[2]; ++dx) {
    
      full_prior_buffer[full_index] = BAD_VAL;
      ++full_index;
    }
  }

} // End evaluate_path SSE
#endif


SemiGlobalMatcher::AccumCostType 
SemiGlobalMatcher::get_accum_vector_min(int col, int row,
                                        DisparityType &dx, DisparityType &dy) {
  // Get the array
  AccumCostType const* vec = get_accum_vector(col, row);
  const int num_disp = get_num_disparities(col, row);
  
  // Get the minimum index of the array
  int min_index = 0;
  AccumCostType value = std::numeric_limits<AccumCostType>::max();
  for (int i=0; i<num_disp; ++i) {
    if (vec[i] < value) {
      value = vec[i];
      min_index = i;
    }
  }
  
  // Convert the disparity index to dx and dy
  const Vector4i bounds = m_disp_bound_image(col,row);
  int d_width  = bounds[2] - bounds[0] + 1;
  dy = (min_index / d_width);
  dx = min_index - (dy*d_width) + bounds[0];
  dy += bounds[1];
  
  //printf("%d, %d, %d -> %d, %d\n", value, min_index, d_width, dx, dy);
  
  return value;
}

SemiGlobalMatcher::DisparityImage
SemiGlobalMatcher::create_disparity_view() {
  // Init output vector
  DisparityImage disparity( m_num_output_cols, m_num_output_rows );
  // For each element in the accumulated costs matrix, 
  //  select the disparity with the lowest accumulated cost.
  //Timer timer("Calculate Disparity Minimum");
  DisparityType dx, dy;
  for ( int j = 0; j < m_num_output_rows; j++ ) {
    for ( int i = 0; i < m_num_output_cols; i++ ) {
      
      int num_disp = get_num_disparities(i, j);
      if (num_disp > 0) {
        // Valid pixel, choose the best disparity with a winner-take-all (WTA) method.
        // - Would a fancier selection method improve our results?
        get_accum_vector_min(i, j, dx, dy);
        disparity(i,j) = DisparityImage::pixel_type(dx, dy);
        
      } else { // Pixels with no search area were never valid.
        disparity(i,j) = DisparityImage::pixel_type();
        invalidate(disparity(i,j));
      }

      /*
      if ((i >= 15) && (i <= 15) && (j==40)) { // DEBUG!
        printf("ACC costs (%d,%d): %d, %d\n", i, j, dx, dy);

        AccumCostType* vec = get_accum_vector(i, j);
        const int num_disp = get_num_disparities(i, j);
        for (int k=0; k<num_disp; ++k)
          std::cout << vec[k] << " " ;
      }
      */
    }
  }
  vw_out(DebugMessage, "stereo") << "Finished creating integer disparity image.\n";
  return disparity;
}



// TODO: Clean up and move!
class HistClass{

public: // Functions
  HistClass(int num_bins, double min, double max): m_num_bins(num_bins), m_min(min), m_max(max) {
    m_data.assign(num_bins, 0);
  };
  
  void add(double val) {
    int bin = static_cast<int>(round( (m_num_bins - 1) * ( (val - m_min)/(m_max - m_min) ) ));
    if ((bin < 0) || (bin >= m_num_bins)) {
      std::cout << "Bad bin = " << bin << ", val = " << val << std::endl;
    }
    else
      m_data[bin]++;
  }
  
  double bin(int i) const {return m_data[i];}
  
  void write(std::string const& path) const {
    std::ofstream f(path.c_str());
    for (int i=0; i<m_num_bins; ++i) {
      f << m_data[i] << std::endl;
    }
    f.close();
  }
  
private: // Functions

  int    m_num_bins;
  double m_min, m_max;
  std::vector<uint64> m_data;
};

// A number of proposed subpixel algorithms
double linearFit(double x) {
  return x/2.0;
}
double parabolaFit(double x) { // Use our 2d implementation instead of this one
  return x/(x+1.0);
}
double poly4Fit(double x) {
  return (x*x*x*x + x)/4.0;
}
double sinFit(double x) {
  const double PI = 3.14159265359; // TODO: MOVE THIS
  return 0.5 * (sin(x*PI/2.0 - PI/2.0) + 1.0);
}
double cosFit(double x) {
  const double PI = 3.14159265359; // TODO: MOVE THIS
  return (1 - cos(x*PI/3.0));
}
double alg16(double x) {
  return std::max(cosFit(x), poly4Fit(x));
}
double lcBlendFit(double x) {
  const double PI = 3.14159265359; // TODO: MOVE THIS
  
  double factor = 1.195 - cos(x*(PI/2.3));
  return cosFit(x)*factor + linearFit(x)*(1.0-factor);
}

/// Add this offset to the integer disparity to get the final result.
double SemiGlobalMatcher::compute_subpixel_offset(AccumCostType prev, AccumCostType center, AccumCostType next) {
  double ld = prev - center;
  double rd = next - center;
  if ((rd == 0) && (ld == 0)) // Handle case where all values are equal
    return 0;
  // Set up computations
  double x = rd/ld;
  double mult = -1.0;
  if (ld < rd) {
    x    = ld/rd;
    mult = 1.0;
  }

  // Use the selected subpixel function
  double value = 0;    
  switch(m_subpixel_type) {
    case SUBPIXEL_POLY4:    value = poly4Fit  (x); break;
    case SUBPIXEL_COSINE:   value = cosFit    (x); break;
    case SUBPIXEL_LC_BLEND: value = lcBlendFit(x); break;
    default:                value = linearFit (x); break;
  };
  // Complete computation
  return (value - 0.5)*mult;
}

double SemiGlobalMatcher::compute_subpixel_ratio(AccumCostType prev, AccumCostType center, AccumCostType next) {
  // Just return the ratio that would be used in compute_subpixel_offset
  // - If this value is written to disk, subpixel functions can be experimented with Matlab or something.
  double ld = prev - center;
  double rd = next - center;
  if ((rd == 0) && (ld == 0))
    return 0;
  if (ld < rd)
    return ld/rd;
  else
    return rd/ld;
}

// Test out alternate subpixel methods
ImageView<PixelMask<Vector2f> > SemiGlobalMatcher::
create_disparity_view_subpixel(DisparityImage const& integer_disparity) {

  //Timer timer("Calculate Subpixel Disparity");

  typedef  PixelMask<Vector2f> p_type;
  ImageView<p_type> disparity(m_num_output_cols, m_num_output_rows);

  ParabolaFit2d fitter; // Only used with parabola2d

  vw_out(DebugMessage, "stereo") << "Creating subpixel disparity image...\n";
  
  // DEBUG
  //HistClass hist_dx(201, -1.0, 1.0), hist_dy(201, -1.0, 1.0);
  //std::ofstream rawFile("raw.csv");
  
  // For each element in the accumulated costs matrix, 
  //  select the disparity with the lowest accumulated cost.
  double percent_bad = 0;
  double delta_x, delta_y;
  for ( int j = 0; j < m_num_output_rows; j++ ) {
    for ( int i = 0; i < m_num_output_cols; i++ ) {
      
      const Vector4i bounds = m_disp_bound_image(i,j);
      //int height   = (bounds[3] - bounds[1] + 1);
      int width    = (bounds[2] - bounds[0] + 1);
      
      // Check the input image to find masked pixels
      PixelMask<Vector2i> integer_pixel = integer_disparity(i, j);
      if (!is_valid(integer_pixel)) {
        disparity(i,j) = integer_pixel;
        invalidate(disparity(i,j));
        continue; // No need for subpixel here
      }      
            
      int dx = integer_pixel[0];
      int dy = integer_pixel[1];

      // Not stop here if not doing subpixel processing      
      if (m_subpixel_type == SUBPIXEL_NONE) {
        disparity(i,j) = p_type(dx, dy);
        continue;
      }
      
      // Linear index of the min offset that will be checked
      int min_index = (dy-bounds[1])*width + (dx-bounds[0]);

      // Fetch the 8 adjacent accumulation vector values

      // Don't interpolate out of bounds
      int x_left  = -1;
      int x_right =  1;
      int y_up    = -width;
      int y_down  =  width;
      if (dx == bounds[0]) x_left  = 0;
      if (dx == bounds[2]) x_right = 0;
      if (dy == bounds[1]) y_up    = 0;
      if (dy == bounds[3]) y_down  = 0;  

      // Apply subpixel correction and apply
      AccumCostType const* accum_vec = get_accum_vector(i, j);


      bool valid = true;
      if (m_subpixel_type == SUBPIXEL_PARABOLA) {
        valid = fitter.find_peak( accum_vec[min_index+x_left+y_up  ],  accum_vec[min_index+y_up  ], accum_vec[min_index+x_right+y_up  ],
                                  accum_vec[min_index+x_left       ],  accum_vec[min_index       ], accum_vec[min_index+x_right       ],
                                  accum_vec[min_index+x_left+y_down],  accum_vec[min_index+y_down], accum_vec[min_index+x_right+y_down],
                                  delta_x, delta_y);
      }
      else {
        // This branch handles all 1D interpolation methods.
        // - These methods are always considered valid.
        delta_x = compute_subpixel_offset(accum_vec[min_index+x_left], accum_vec[min_index], accum_vec[min_index+x_right]);
        delta_y = compute_subpixel_offset(accum_vec[min_index+y_up  ], accum_vec[min_index], accum_vec[min_index+y_down ]);
      }

      // To assist development, write the internal subpixel input ratio to a file.
      //double temp = compute_subpixel_ratio(accum_vec[min_index+x_left], accum_vec[min_index], accum_vec[min_index+x_right]);
      //rawFile << temp << std::endl;

      if (valid) {
        disparity(i,j) = p_type(dx+delta_x, dy+delta_y);
        //hist_dx.add(delta_x);
        //hist_dy.add(delta_y);
      }
      else {
        disparity(i,j) = p_type(dx, dy);
        percent_bad += 1.0;
      }
    } // End col loop
  } // End row loop
  
  percent_bad /= (double)(m_num_output_rows*m_num_output_cols);
  vw_out(DebugMessage, "stereo") << "Subpixel interpolation failure percentage: " << percent_bad << std::endl;

  // Write these out for debugging/development
  //write_image( "subpixel_disp.tif", disparity );
  //hist_dx.write("delta_x.csv");
  //hist_dy.write("delta_y.csv");
  //rawFile.close();
  
  return disparity;
  
}




// TODO: Replace with ASP implementation?
SemiGlobalMatcher::CostType SemiGlobalMatcher::get_cost_block(ImageView<uint8> const& left_image,
               ImageView<uint8> const& right_image,
               int left_x, int left_y, int right_x, int right_y, bool debug) {
  if (m_kernel_size == 1) { // Special handling for single pixel case
    int diff = static_cast<int>(left_image (left_x,  left_y )) - 
               static_cast<int>(right_image(right_x, right_y));
    return static_cast<CostType>(abs(diff));
  }

  // Block mean of abs dists
  const int half_kernel_size = (m_kernel_size-1) / 2;
  int sum=0, diff=0;
  for (int j=-half_kernel_size; j<=half_kernel_size; ++j) {
    for (int i=-half_kernel_size; i<=half_kernel_size; ++i) {
      diff = static_cast<int>(left_image (left_x +i, left_y +j)) - 
             static_cast<int>(right_image(right_x+i, right_y+j));
      sum += abs(diff);
    }
  }
  CostType result = sum / static_cast<int>(m_kernel_size*m_kernel_size);
  //printf("sum = %d, result = %d\n", sum, result);
  return static_cast<CostType>(result);
}

void SemiGlobalMatcher::fill_costs_block(ImageView<uint8> const& left_image,
                                         ImageView<uint8> const& right_image){
  // Make sure we don't go out of bounds here due to the disparity shift and kernel.
  size_t cost_index = 0;
  for ( int r = m_min_row; r <= m_max_row; r++ ) { // For each row in left
    int output_row = r - m_min_row;
    //int input_row  = r;
    for ( int c = m_min_col; c <= m_max_col; c++ ) { // For each column in left
      int output_col = c - m_min_col;
      //int input_col  = c;
      
      Vector4i pixel_disp_bounds = m_disp_bound_image(output_col, output_row);

      // Only compute costs in the search radius for this pixel    
      for ( int dy = pixel_disp_bounds[1]; dy <= pixel_disp_bounds[3]; dy++ ) { // For each disparity
        for ( int dx = pixel_disp_bounds[0]; dx <= pixel_disp_bounds[2]; dx++ ) {          
          
          CostType cost = get_cost_block(left_image, right_image, c, r, c+dx,r+dy, false);
          m_cost_buffer[cost_index] = cost;
          ++cost_index;
        }    
      } // End disparity loops   
    } // End x loop
  }// End y loop
  
  
}


void SemiGlobalMatcher::fill_costs_census3x3(ImageView<uint8> const& left_image,
                                             ImageView<uint8> const& right_image){
  const int half_kernel = (m_kernel_size - 1) / 2;
  const int padding     = 2*half_kernel;
                                                
  // Compute the census value for each pixel.
  // - ROI handling could be fancier but this is simple and works.
  // - The 0,0 pixels in the left and right images are assumed to be aligned.
  ImageView<uint8> left_census (left_image.cols()-padding,  left_image.rows()-padding ), 
                   right_census(right_image.cols()-padding, right_image.rows()-padding);

  if (m_cost_type == CENSUS_TRANSFORM) {
    for ( int r = 0; r < left_census.rows(); r++ )
      for ( int c = 0; c < left_census.cols(); c++ )
        left_census(c,r) = get_census_value_3x3(left_image, c+half_kernel, r+half_kernel);
    for ( int r = 0; r < right_census.rows(); r++ )
      for ( int c = 0; c < right_census.cols(); c++ )
        right_census(c,r) = get_census_value_3x3(right_image, c+half_kernel, r+half_kernel);
  } else {
    vw_throw(NoImplErr() << "The ternary sensus transform not available in size 3!\n");
  } 
  get_hamming_distance_costs(left_census, right_census);
}

void SemiGlobalMatcher::fill_costs_census5x5(ImageView<uint8> const& left_image,
                                             ImageView<uint8> const& right_image){
  const int half_kernel = (m_kernel_size - 1) / 2;
  const int padding     = 2*half_kernel;
  
  // Compute the census value for each pixel.
  // - ROI handling could be fancier but this is simple and works.
  // - The 0,0 pixels in the left and right images are assumed to be aligned.
  ImageView<uint32> left_census (left_image.cols()-padding,  left_image.rows()-padding ), 
                    right_census(right_image.cols()-padding, right_image.rows()-padding);

  if (m_cost_type == CENSUS_TRANSFORM) {
    for ( int r = 0; r < left_census.rows(); r++ )
      for ( int c = 0; c < left_census.cols(); c++ )
        left_census(c,r) = get_census_value_5x5(left_image, c+half_kernel, r+half_kernel);
    for ( int r = 0; r < right_census.rows(); r++ )
      for ( int c = 0; c < right_census.cols(); c++ )
        right_census(c,r) = get_census_value_5x5(right_image, c+half_kernel, r+half_kernel);
  } else { // TERNARY_CENSUS_TRANSFORM
    for ( int r = 0; r < left_census.rows(); r++ )
      for ( int c = 0; c < left_census.cols(); c++ )
        left_census(c,r) = get_census_value_ternary_5x5(left_image, c+half_kernel, r+half_kernel, m_ternary_census_threshold);
    for ( int r = 0; r < right_census.rows(); r++ )
      for ( int c = 0; c < right_census.cols(); c++ )
        right_census(c,r) = get_census_value_ternary_5x5(right_image, c+half_kernel, r+half_kernel, m_ternary_census_threshold);
  }
  
  get_hamming_distance_costs(left_census, right_census);
}

void SemiGlobalMatcher::fill_costs_census7x7(ImageView<uint8> const& left_image,
                                             ImageView<uint8> const& right_image){
  const int half_kernel = (m_kernel_size - 1) / 2;
  const int padding     = 2*half_kernel;
  
  // Compute the census value for each pixel.
  // - ROI handling could be fancier but this is simple and works.
  // - The 0,0 pixels in the left and right images are assumed to be aligned.
  ImageView<uint64> left_census (left_image.cols()-padding,  left_image.rows()-padding ), 
                    right_census(right_image.cols()-padding, right_image.rows()-padding);
                   
  if (m_cost_type == CENSUS_TRANSFORM) {
    for ( int r = 0; r < left_census.rows(); r++ )
      for ( int c = 0; c < left_census.cols(); c++ )
        left_census(c,r) = get_census_value_7x7(left_image, c+half_kernel, r+half_kernel);
    for ( int r = 0; r < right_census.rows(); r++ )
      for ( int c = 0; c < right_census.cols(); c++ )
        right_census(c,r) = get_census_value_7x7(right_image, c+half_kernel, r+half_kernel);
  } else { // TERNARY_CENSUS_TRANSFORM
    for ( int r = 0; r < left_census.rows(); r++ )
      for ( int c = 0; c < left_census.cols(); c++ )
        left_census(c,r) = get_census_value_ternary_7x7(left_image, c+half_kernel, r+half_kernel, m_ternary_census_threshold);
    for ( int r = 0; r < right_census.rows(); r++ )
      for ( int c = 0; c < right_census.cols(); c++ )
        right_census(c,r) = get_census_value_ternary_7x7(right_image, c+half_kernel, r+half_kernel, m_ternary_census_threshold);
  }
  
  get_hamming_distance_costs(left_census, right_census);
}

void SemiGlobalMatcher::fill_costs_census9x9(ImageView<uint8> const& left_image,
                                             ImageView<uint8> const& right_image){
  const int half_kernel = (m_kernel_size - 1) / 2;
  const int padding     = 2*half_kernel;
  
  // Compute the census value for each pixel.
  // - ROI handling could be fancier but this is simple and works.
  // - The 0,0 pixels in the left and right images are assumed to be aligned.
  ImageView<uint64> left_census (left_image.cols()-padding,  left_image.rows()-padding ), 
                    right_census(right_image.cols()-padding, right_image.rows()-padding);
                   
  if (m_cost_type == CENSUS_TRANSFORM) {
    vw_throw(NoImplErr() << "The Census transform not available in size 9!\n");
  } else { // TERNARY_CENSUS_TRANSFORM
    for ( int r = 0; r < left_census.rows(); r++ )
      for ( int c = 0; c < left_census.cols(); c++ )
        left_census(c,r) = get_census_value_ternary_9x9(left_image, c+half_kernel, r+half_kernel, m_ternary_census_threshold);
    for ( int r = 0; r < right_census.rows(); r++ )
      for ( int c = 0; c < right_census.cols(); c++ )
        right_census(c,r) = get_census_value_ternary_9x9(right_image, c+half_kernel, r+half_kernel, m_ternary_census_threshold);
  }
  
  get_hamming_distance_costs(left_census, right_census);
}

// TODO: Add multithreading capability to this function!
void SemiGlobalMatcher::compute_disparity_costs(ImageView<uint8> const& left_image,
                                                ImageView<uint8> const& right_image) {  
  //Timer timer("\tSGM Cost Calculation");
  if ((m_cost_type == CENSUS_TRANSFORM) || (m_cost_type == TERNARY_CENSUS_TRANSFORM)) {
    switch(m_kernel_size) {
    case 3:  fill_costs_census3x3(left_image, right_image); break;
    case 5:  fill_costs_census5x5(left_image, right_image); break;
    case 7:  fill_costs_census7x7(left_image, right_image); break;
    case 9:  fill_costs_census9x9(left_image, right_image); break;
    default: vw_throw( NoImplErr() << "Census transform is only available in size 3, 5, and 7!\n" );
    };
  }
  else { // Use the default mean of diff cost function
    // Replace this with ASP's efficient existing cost functions?
    fill_costs_block(left_image, right_image);
  }
  
/*
 int debug_row = m_num_output_rows * 0.5;
 if (m_num_output_rows > debug_row) {
   std::cout << "debug row = " << debug_row << std::endl;
   ImageView<uint8> cost_image( m_num_output_cols, m_num_disp ); // TODO: Change type?
   std::fill(cost_image.data(), cost_image.data()+m_num_output_cols*m_num_disp, 255);
   for ( int i = 0; i < m_num_output_cols; i++ ) {
     CostType* buff = get_cost_vector(i, debug_row);
     Vector4i bounds = m_disp_bound_image(i,debug_row);
     int index = 0;
     for ( int dy = bounds[1]; dy < bounds[3]; dy++ ) {
       for ( int dx = bounds[0]; dx < bounds[2]; dx++ ) {
         int d = xy_to_disp(dx, dy);
         //std::cout << "cost = " << int(buff[d]) << std::endl;
         cost_image(i,d) = buff[index]; // TODO: scale!
         ++index;
       }
     }
   }
   write_image("scanline_costs_block.tif",cost_image);
   std::cout << "Done writing line dump.\n";
 }
*/

} // end compute_disparity_costs() 




void SemiGlobalMatcher::two_trip_path_accumulation(ImageView<uint8> const& left_image) {

  //Timer timer_total("\tSGM Cost Propagation");

  /// Create an object to manage the temporary accumulation buffers that need to be used here.
  MultiAccumRowBuffer buff_manager(this);
  
  // Init this buffer to bad scores representing disparities that were
  //  not in the search range for the given pixel.
  boost::shared_array<AccumCostType> full_prior_buffer;
  full_prior_buffer.reset(new AccumCostType[m_num_disp]);
  for (int i=0; i<m_num_disp; ++i)
    full_prior_buffer[i] = get_bad_accum_val();  

  AccumCostType* full_prior_ptr = full_prior_buffer.get();
  AccumCostType* output_accum_ptr;
  const int last_column = m_num_output_cols - 1;
  const int last_row    = m_num_output_rows - 1;

  // Loop through all pixels in the output image for the first trip, top-left to bottom-right.
  for (int row=0; row<m_num_output_rows; ++row) {
    for (int col=0; col<m_num_output_cols; ++col) {
    
      //printf("Accum pass 1 col = %d, row = %d\n", col, row);
    
      int num_disp = get_num_disparities(col, row);
      CostType * const local_cost_ptr = get_cost_vector(col, row);
      bool debug = false;//(col==152) && (row == 12);
      
      // Top left
      output_accum_ptr = buff_manager.get_output_accum_ptr(MultiAccumRowBuffer::TOP_LEFT);
      if ((row > 0) && (col > 0)) {
        // Fill in the accumulated value in the bottom buffer
        int pixel_diff = get_path_pixel_diff(left_image, col, row, 1, 1);
        AccumCostType* const prior_accum_ptr = buff_manager.get_trailing_pixel_accum_ptr(-1, -1, MultiAccumRowBuffer::TOP_LEFT);
        evaluate_path( col, row, col-1, row-1,
                       prior_accum_ptr, full_prior_ptr, local_cost_ptr, output_accum_ptr, 
                       pixel_diff, debug );
      }
      else // Just init to the local cost
        for (int d=0; d<num_disp; ++d) output_accum_ptr[d] = local_cost_ptr[d];

      // Top
      output_accum_ptr = buff_manager.get_output_accum_ptr(MultiAccumRowBuffer::TOP);
      if (row > 0) {
        int pixel_diff = get_path_pixel_diff(left_image, col, row, 0, 1);
        AccumCostType* const prior_accum_ptr = buff_manager.get_trailing_pixel_accum_ptr(0, -1, MultiAccumRowBuffer::TOP);
        evaluate_path( col, row, col, row-1,
                       prior_accum_ptr, full_prior_ptr, local_cost_ptr, output_accum_ptr, 
                       pixel_diff, debug );
      }
      else // Just init to the local cost
        for (int d=0; d<num_disp; ++d) output_accum_ptr[d] = local_cost_ptr[d];
      
      // Top right
      output_accum_ptr = buff_manager.get_output_accum_ptr(MultiAccumRowBuffer::TOP_RIGHT);
      if ((row > 0) && (col < last_column)) {
        int pixel_diff = get_path_pixel_diff(left_image, col, row, -1, 1);
        AccumCostType* const prior_accum_ptr = buff_manager.get_trailing_pixel_accum_ptr(1, -1, MultiAccumRowBuffer::TOP_RIGHT);
        evaluate_path( col, row, col+1, row-1,
                       prior_accum_ptr, full_prior_ptr, local_cost_ptr, output_accum_ptr, 
                       pixel_diff, debug );
      }
      else // Just init to the local cost
        for (int d=0; d<num_disp; ++d) output_accum_ptr[d] = local_cost_ptr[d];
      
      // Left
      output_accum_ptr = buff_manager.get_output_accum_ptr(MultiAccumRowBuffer::LEFT);
      if (col > 0) {
        int pixel_diff = get_path_pixel_diff(left_image, col, row, 1, 0);
        AccumCostType* const prior_accum_ptr = buff_manager.get_trailing_pixel_accum_ptr(-1, 0, MultiAccumRowBuffer::LEFT);
        evaluate_path( col, row, col-1, row,
                       prior_accum_ptr, full_prior_ptr, local_cost_ptr, output_accum_ptr, 
                       pixel_diff, debug );
      }
      else // Just init to the local cost
        for (int d=0; d<num_disp; ++d) output_accum_ptr[d] = local_cost_ptr[d];

      buff_manager.next_pixel();
    } // End col loop
    
    buff_manager.next_row(row==m_num_output_rows-1);
  } // End row loop
  
  // Done with the first trip!
  buff_manager.switch_trips();

  // Loop through all pixels in the output image for the first trip, bottom-right to top-left.
  for (int row = last_row; row >= 0; --row) {
    for (int col = last_column; col >= 0; --col) {
    
      int num_disp = get_num_disparities(col, row);
      CostType * const local_cost_ptr = get_cost_vector(col, row);
      bool debug = false;//((row == 244) && (col == 341));

      // Bottom right
      output_accum_ptr = buff_manager.get_output_accum_ptr(MultiAccumRowBuffer::BOT_RIGHT);
      if ((row < last_row) && (col < last_column)) {
        // Fill in the accumulated value in the bottom buffer
        int pixel_diff = get_path_pixel_diff(left_image, col, row, -1, -1);
        AccumCostType* const prior_accum_ptr = buff_manager.get_trailing_pixel_accum_ptr(1, 1, MultiAccumRowBuffer::BOT_RIGHT);
        evaluate_path( col, row, col+1, row+1,
                       prior_accum_ptr, full_prior_ptr, local_cost_ptr, output_accum_ptr, 
                       pixel_diff, debug );
      }
      else // Just init to the local cost
        for (int d=0; d<num_disp; ++d) output_accum_ptr[d] = local_cost_ptr[d];
      
      // Bottom
      output_accum_ptr = buff_manager.get_output_accum_ptr(MultiAccumRowBuffer::BOT);
      if (row < last_row) {
        int pixel_diff = get_path_pixel_diff(left_image, col, row, 0, -1);
        AccumCostType* const prior_accum_ptr = buff_manager.get_trailing_pixel_accum_ptr(0, 1, MultiAccumRowBuffer::BOT);
        evaluate_path( col, row, col, row+1,
                       prior_accum_ptr, full_prior_ptr, local_cost_ptr, output_accum_ptr, 
                       pixel_diff, debug );
      }
      else // Just init to the local cost
        for (int d=0; d<num_disp; ++d) output_accum_ptr[d] = local_cost_ptr[d];

      // Bottom left
      output_accum_ptr = buff_manager.get_output_accum_ptr(MultiAccumRowBuffer::BOT_LEFT);
      if ((row < last_row) && (col > 0)) {
        int pixel_diff = get_path_pixel_diff(left_image, col, row, 1, -1);
        AccumCostType* const prior_accum_ptr = buff_manager.get_trailing_pixel_accum_ptr(-1, 1, MultiAccumRowBuffer::BOT_LEFT);
        evaluate_path( col, row, col-1, row+1,
                       prior_accum_ptr, full_prior_ptr, local_cost_ptr, output_accum_ptr, 
                       pixel_diff, debug );
      }
      else // Just init to the local cost
        for (int d=0; d<num_disp; ++d) output_accum_ptr[d] = local_cost_ptr[d];

      // Right
      output_accum_ptr = buff_manager.get_output_accum_ptr(MultiAccumRowBuffer::RIGHT);
      if (col < last_column) {
        int pixel_diff = get_path_pixel_diff(left_image, col, row, -1, 0);
        AccumCostType* const prior_accum_ptr = buff_manager.get_trailing_pixel_accum_ptr(1, 0, MultiAccumRowBuffer::RIGHT);
        evaluate_path( col, row, col+1, row,
                       prior_accum_ptr, full_prior_ptr, local_cost_ptr, output_accum_ptr, 
                       pixel_diff, debug );
      }
      else // Just init to the local cost
        for (int d=0; d<num_disp; ++d) output_accum_ptr[d] = local_cost_ptr[d];

      buff_manager.next_pixel();
    } // End col loop
    
    buff_manager.next_row(row==0);    
  } // End row loop

  // Done with both trips!
}





// This version of the function requires four passes and is based on the paper:
// MGM: A Significantly More Global Matching for Stereovision
void SemiGlobalMatcher::smooth_path_accumulation(ImageView<uint8> const& left_image) {

  //Timer timer_total("\tSGM Cost Propagation");

  const int PATHS_PER_PASS = 2;

  // Create objects to manage the temporary accumulation buffers that need to be used here.
  // - Two copies are needed here, one for the two horizontal passes and one for the two vertical passes
  MultiAccumRowBuffer buff_manager_horizontal(this, PATHS_PER_PASS, false);
  MultiAccumRowBuffer buff_manager_vertical  (this, PATHS_PER_PASS, true);
  
  // Init this buffer to bad scores representing disparities that were
  //  not in the search range for the given pixel.
  boost::shared_array<AccumCostType> full_prior_buffer;
  full_prior_buffer.reset(new AccumCostType[m_num_disp]);
  for (int i=0; i<m_num_disp; ++i)
    full_prior_buffer[i] = get_bad_accum_val();  

  AccumCostType* full_prior_ptr = full_prior_buffer.get();
  AccumCostType* output_accum_ptr;
  const int last_column = m_num_output_cols - 1;
  const int last_row    = m_num_output_rows - 1;

  std::cout << "Starting first trip...\n";

  // Loop through all pixels in the output image for the first trip, top-left to bottom-right.
  for (int row=0; row<m_num_output_rows; ++row) {
    for (int col=0; col<m_num_output_cols; ++col) {
    
      //printf("Accum pass 1 col = %d, row = %d\n", col, row);
    
      // TODO: Do we need to skip these pixels in the two-pass SGM function above?
      int num_disp = get_num_disparities(col, row);
      if (num_disp == 0) {
        buff_manager_horizontal.next_pixel();
        continue;
      }
      CostType * const local_cost_ptr = get_cost_vector(col, row);
      bool debug = false;//((row == 244) && (col == 341));
      
      boost::shared_array<AccumCostType> temp_buffer(new AccumCostType[num_disp]);

      // Left
      output_accum_ptr = buff_manager_horizontal.get_output_accum_ptr(MultiAccumRowBuffer::PASS_ONE);
      if ((row > 0) && (col > 0)) {
        int pixel_diff = get_path_pixel_diff(left_image, col, row, -1, 0);
        // Compute accumulation from the values in the left pixel
        AccumCostType* const prior_accum_ptr = buff_manager_horizontal.get_trailing_pixel_accum_ptr(-1, 0, MultiAccumRowBuffer::PASS_ONE);
        evaluate_path( col, row, col-1, row,
                       prior_accum_ptr, full_prior_ptr, local_cost_ptr, output_accum_ptr, 
                       pixel_diff, debug );
                       
        // Compute accumulation from the values in the above pixel
        AccumCostType* const prior_accum_ptr2 = buff_manager_horizontal.get_trailing_pixel_accum_ptr(0, -1, MultiAccumRowBuffer::PASS_ONE);
        evaluate_path( col, row, col, row-1,
                       prior_accum_ptr2, full_prior_ptr, local_cost_ptr, temp_buffer.get(), 
                       pixel_diff, debug );
        // The final accumulation values are the average of the two computations
        for (int d=0; d<num_disp; ++d)
          output_accum_ptr[d] = (output_accum_ptr[d] + temp_buffer[d])/2;
      }
      else // Just init to the local cost
        for (int d=0; d<num_disp; ++d) output_accum_ptr[d] = local_cost_ptr[d];
        
      // Top left
      output_accum_ptr = buff_manager_horizontal.get_output_accum_ptr(MultiAccumRowBuffer::PASS_TWO);
      if ((row > 0) && (col > 0) && (col < last_column)) {

        int pixel_diff = get_path_pixel_diff(left_image, col, row, -1, -1);
        AccumCostType* const prior_accum_ptr = buff_manager_horizontal.get_trailing_pixel_accum_ptr(-1, -1, MultiAccumRowBuffer::PASS_TWO);
        evaluate_path( col, row, col-1, row-1,
                       prior_accum_ptr, full_prior_ptr, local_cost_ptr, output_accum_ptr, 
                       pixel_diff, debug );
                       
        AccumCostType* const prior_accum_ptr2 = buff_manager_horizontal.get_trailing_pixel_accum_ptr(1, -1, MultiAccumRowBuffer::PASS_TWO);
        evaluate_path( col, row, col+1, row-1,
                       prior_accum_ptr2, full_prior_ptr, local_cost_ptr, temp_buffer.get(), 
                       pixel_diff, debug );
        for (int d=0; d<num_disp; ++d)
          output_accum_ptr[d] = (output_accum_ptr[d] + temp_buffer[d])/2;
      }
      else // Just init to the local cost
        for (int d=0; d<num_disp; ++d) output_accum_ptr[d] = local_cost_ptr[d];

      buff_manager_horizontal.next_pixel();
    } // End col loop
    
    buff_manager_horizontal.next_row(row==last_row);
  } // End row loop

  
  // Done with the first trip!
  buff_manager_horizontal.switch_trips();  


  std::cout << "Starting second trip...\n";

  // Loop through all pixels in the output image for the second trip, bottom-right to top-left.
  for (int row = last_row; row >= 0; --row) {
    for (int col = last_column; col >= 0; --col) {
    
      int num_disp = get_num_disparities(col, row);
      if (num_disp == 0) {
        buff_manager_horizontal.next_pixel();
        continue;
      }
      CostType * const local_cost_ptr = get_cost_vector(col, row);
      bool debug = false;//((row == 244) && (col == 341));
      
      boost::shared_array<AccumCostType> temp_buffer(new AccumCostType[num_disp]);

      // Right
      output_accum_ptr = buff_manager_horizontal.get_output_accum_ptr(MultiAccumRowBuffer::PASS_ONE);
      if ((row < last_row) && (col < last_column)) {
        int pixel_diff = get_path_pixel_diff(left_image, col, row, 1, 0);
        AccumCostType* const prior_accum_ptr = buff_manager_horizontal.get_trailing_pixel_accum_ptr(1, 0, MultiAccumRowBuffer::PASS_ONE);
        evaluate_path( col, row, col+1, row,
                       prior_accum_ptr, full_prior_ptr, local_cost_ptr, output_accum_ptr, 
                       pixel_diff, debug );
                       
        AccumCostType* const prior_accum_ptr2 = buff_manager_horizontal.get_trailing_pixel_accum_ptr(0, 1, MultiAccumRowBuffer::PASS_ONE);
        evaluate_path( col, row, col, row+1,
                       prior_accum_ptr2, full_prior_ptr, local_cost_ptr, temp_buffer.get(), 
                       pixel_diff, debug );
        for (int d=0; d<num_disp; ++d)
          output_accum_ptr[d] = (output_accum_ptr[d] + temp_buffer[d])/2;                      
      }
      else // Just init to the local cost
        for (int d=0; d<num_disp; ++d) output_accum_ptr[d] = local_cost_ptr[d];

      
      // Bottom right
      output_accum_ptr = buff_manager_horizontal.get_output_accum_ptr(MultiAccumRowBuffer::PASS_TWO);
      if ((row < last_row) && (col > 0) && (col < last_column)) {

        int pixel_diff = get_path_pixel_diff(left_image, col, row, 1, 1);
        AccumCostType* const prior_accum_ptr = buff_manager_horizontal.get_trailing_pixel_accum_ptr(1, 1, MultiAccumRowBuffer::PASS_TWO);
        evaluate_path( col, row, col+1, row+1,
                       prior_accum_ptr, full_prior_ptr, local_cost_ptr, output_accum_ptr, 
                       pixel_diff, debug );

        AccumCostType* const prior_accum_ptr2 = buff_manager_horizontal.get_trailing_pixel_accum_ptr(-1, 1, MultiAccumRowBuffer::PASS_TWO);
        evaluate_path( col, row, col-1, row+1,
                       prior_accum_ptr2, full_prior_ptr, local_cost_ptr, temp_buffer.get(), 
                       pixel_diff, debug );
        for (int d=0; d<num_disp; ++d)
          output_accum_ptr[d] = (output_accum_ptr[d] + temp_buffer[d])/2;
      }
      else // Just init to the local cost
        for (int d=0; d<num_disp; ++d) output_accum_ptr[d] = local_cost_ptr[d];


      buff_manager_horizontal.next_pixel();
    } // End col loop
    
    buff_manager_horizontal.next_row(row==0);    
  } // End row loop

  // -- Done with the horizontal passes, switch to vertical passes.

  std::cout << "Starting third trip...\n";

  // Loop through all pixels in the output image for the third trip, bottom-left to top-right.
  for (int col = 0; col < m_num_output_cols; ++col) {
    for (int row = last_row; row >= 0; --row) {
    
      int num_disp = get_num_disparities(col, row);
      //printf("Accum pass 3 col = %d, row = %d, num_disp = %d\n", col, row, num_disp);
      if (num_disp == 0) {
        buff_manager_vertical.next_pixel();
        continue;
      }
      CostType * const local_cost_ptr = get_cost_vector(col, row);
      bool debug = false;//((row == 244) && (col == 341));
     
      boost::shared_array<AccumCostType> temp_buffer(new AccumCostType[num_disp]);
      
      // Bottom
      output_accum_ptr = buff_manager_vertical.get_output_accum_ptr(MultiAccumRowBuffer::PASS_ONE);
      if ((row < last_row) && (col > 0)) {
        int pixel_diff = get_path_pixel_diff(left_image, col, row, 0, 1);
        AccumCostType* const prior_accum_ptr = buff_manager_vertical.get_trailing_pixel_accum_ptr(0, 1, MultiAccumRowBuffer::PASS_ONE);
        evaluate_path( col, row, col, row+1,
                       prior_accum_ptr, full_prior_ptr, local_cost_ptr, output_accum_ptr, 
                       pixel_diff, debug );
                       
        AccumCostType* const prior_accum_ptr2 = buff_manager_vertical.get_trailing_pixel_accum_ptr(-1, 0, MultiAccumRowBuffer::PASS_ONE);
        evaluate_path( col, row, col-1, row,
                       prior_accum_ptr2, full_prior_ptr, local_cost_ptr, temp_buffer.get(), 
                       pixel_diff, debug );
        for (int d=0; d<num_disp; ++d)
          output_accum_ptr[d] = (output_accum_ptr[d] + temp_buffer[d])/2;
      }
      else // Just init to the local cost
        for (int d=0; d<num_disp; ++d) output_accum_ptr[d] = local_cost_ptr[d];
      
      // Bottom left
      output_accum_ptr = buff_manager_vertical.get_output_accum_ptr(MultiAccumRowBuffer::PASS_TWO);
      if ((row > 0) && (row < last_row) && (col > 0)) {
        int pixel_diff = get_path_pixel_diff(left_image, col, row, -1, 1);
        AccumCostType* const prior_accum_ptr = buff_manager_vertical.get_trailing_pixel_accum_ptr(-1, 1, MultiAccumRowBuffer::PASS_TWO);
        evaluate_path( col, row, col-1, row+1,
                       prior_accum_ptr, full_prior_ptr, local_cost_ptr, output_accum_ptr, 
                       pixel_diff, debug );
                       
        AccumCostType* const prior_accum_ptr2 = buff_manager_vertical.get_trailing_pixel_accum_ptr(-1, -1, MultiAccumRowBuffer::PASS_TWO);
        evaluate_path( col, row, col-1, row-1,
                       prior_accum_ptr2, full_prior_ptr, local_cost_ptr, temp_buffer.get(), 
                       pixel_diff, debug );
        for (int d=0; d<num_disp; ++d)
          output_accum_ptr[d] = (output_accum_ptr[d] + temp_buffer[d])/2;
      }
      else // Just init to the local cost
        for (int d=0; d<num_disp; ++d) output_accum_ptr[d] = local_cost_ptr[d];
      
      buff_manager_vertical.next_pixel();
    } // End col loop
    
    buff_manager_vertical.next_row(col==last_column);    
  } // End row loop

  // Done with the first trip!
  buff_manager_vertical.switch_trips();

  std::cout << "Starting fourth trip...\n";

  // Loop through all pixels in the output image for the fourth trip, top-right to bottom-left.
  for (int col=last_column; col>=0; --col) {
    for (int row=0; row<m_num_output_rows; ++row) {
    
      //printf("Accum pass 1 col = %d, row = %d\n", col, row);
    
      int num_disp = get_num_disparities(col, row);
      if (num_disp == 0) {
        buff_manager_vertical.next_pixel();
        continue;
      }
      CostType * const local_cost_ptr = get_cost_vector(col, row);
      bool debug = false;//((row == 244) && (col == 341));
      
      boost::shared_array<AccumCostType> temp_buffer(new AccumCostType[num_disp]);
      
      // Top
      output_accum_ptr = buff_manager_vertical.get_output_accum_ptr(MultiAccumRowBuffer::PASS_ONE);
      if ((row > 0) && (col < last_column)) {
        int pixel_diff = get_path_pixel_diff(left_image, col, row, 0, -1);
        AccumCostType* const prior_accum_ptr = buff_manager_vertical.get_trailing_pixel_accum_ptr(0, -1, MultiAccumRowBuffer::PASS_ONE);
        evaluate_path( col, row, col, row-1,
                       prior_accum_ptr, full_prior_ptr, local_cost_ptr, output_accum_ptr, 
                       pixel_diff, debug );
                       
        AccumCostType* const prior_accum_ptr2 = buff_manager_vertical.get_trailing_pixel_accum_ptr(1, 0, MultiAccumRowBuffer::PASS_ONE);
        evaluate_path( col, row, col+1, row,
                       prior_accum_ptr2, full_prior_ptr, local_cost_ptr, temp_buffer.get(), 
                       pixel_diff, debug );
        for (int d=0; d<num_disp; ++d)
          output_accum_ptr[d] = (output_accum_ptr[d] + temp_buffer[d])/2;
      }
      else // Just init to the local cost
        for (int d=0; d<num_disp; ++d) output_accum_ptr[d] = local_cost_ptr[d];
      
      // Top right
      output_accum_ptr = buff_manager_vertical.get_output_accum_ptr(MultiAccumRowBuffer::PASS_TWO);
      if ((row > 0) && (row < last_row) && (col < last_column)) {
        int pixel_diff = get_path_pixel_diff(left_image, col, row, 1, -1);
        AccumCostType* const prior_accum_ptr = buff_manager_vertical.get_trailing_pixel_accum_ptr(1, -1, MultiAccumRowBuffer::PASS_TWO);
        evaluate_path( col, row, col+1, row-1,
                       prior_accum_ptr, full_prior_ptr, local_cost_ptr, output_accum_ptr, 
                       pixel_diff, debug );
                       
        AccumCostType* const prior_accum_ptr2 = buff_manager_vertical.get_trailing_pixel_accum_ptr(1, 1, MultiAccumRowBuffer::PASS_TWO);
        evaluate_path( col, row, col+1, row+1,
                       prior_accum_ptr2, full_prior_ptr, local_cost_ptr, temp_buffer.get(), 
                       pixel_diff, debug );
        for (int d=0; d<num_disp; ++d)
          output_accum_ptr[d] = (output_accum_ptr[d] + temp_buffer[d])/2;
      }
      else // Just init to the local cost
        for (int d=0; d<num_disp; ++d) output_accum_ptr[d] = local_cost_ptr[d];
      

      buff_manager_vertical.next_pixel();
    } // End col loop
    
    buff_manager_vertical.next_row(col==0);
  } // End row loop
  

  // Done with all trips!
} // End function smooth_path_accumulation




SemiGlobalMatcher::DisparityImage
SemiGlobalMatcher::semi_global_matching_func( ImageView<uint8> const& left_image,
                                              ImageView<uint8> const& right_image,
                                              ImageView<uint8> const* left_image_mask,
                                              ImageView<uint8> const* right_image_mask,
                                              DisparityImage const* prev_disparity) {
                                              
  // Compute safe bounds to search through given the disparity range and kernel size.
  // - Using inclusive bounds here.
  
  const int half_kernel_size = (m_kernel_size-1) / 2;

  // Figure out the maximum possible image extent that we can compute stereo for
  //  given the size of two input images and the requirement that we fully search
  //  the specified search range.
  // - The search region = the region in the left image + the search bounds, shrunk by
  //   half the cost function kernel size.
  m_min_row = half_kernel_size - m_min_disp_y; // Assumes the (0,0) pixels are aligned
  m_min_col = half_kernel_size - m_min_disp_x;
  int left_last_col  = left_image.cols () - 1;
  int left_last_row  = left_image.rows () - 1;
  int right_last_col = right_image.cols() - 1;
  int right_last_row = right_image.rows() - 1;
  m_max_row = std::min(left_last_row  -  half_kernel_size,
                       right_last_row - (half_kernel_size + m_max_disp_y));
  m_max_col = std::min(left_last_col  - half_kernel_size,
                       right_last_col - (half_kernel_size + m_max_disp_x));
  if (m_min_row < 0) m_min_row = 0;
  if (m_min_col < 0) m_min_col = 0;
  if (m_max_row > left_last_row) m_max_row = left_last_row;
  if (m_max_col > left_last_col) m_max_col = left_last_col;

  m_num_output_cols  = m_max_col - m_min_col + 1;
  m_num_output_rows  = m_max_row - m_min_row + 1;

  vw_out(DebugMessage, "stereo") << "Computed SGM cost bounding box: " << std::endl;
  vw_out(DebugMessage, "stereo") << "Left image size = ("<<left_image.cols()<<","<<left_image.rows()
                               <<"), right image size = ("<<right_image.cols()<<","<<right_image.rows()<<")\n";
  vw_out(DebugMessage, "stereo") << "min_row = "<< m_min_row <<", min_col = "<< m_min_col <<
                                  ", max_row = "<< m_max_row <<", max_col = "<< m_max_col <<
                                  ", output_height = "<< m_num_output_rows <<
                                  ", output_width = "<< m_num_output_cols <<"\n";

  populate_adjacent_disp_lookup_table();

  // By default the search bounds are the same for each pixel,
  //  but set them from the prior disparity image if the user passed it in.
  populate_constant_disp_bound_image();
  
  if (!populate_disp_bound_image(left_image_mask, right_image_mask, prev_disparity)) {
    vw_out(WarningMessage, "stereo") << "No valid pixels found in SGM input!.\n";
    // If the inputs are invalid, return a default disparity image.
    DisparityImage disparity( m_num_output_cols, m_num_output_rows );
    return invalidate_mask(disparity);
  }

  // All the hard work is done in the next few function calls!

  allocate_large_buffers();

  compute_disparity_costs(left_image, right_image);

  if (m_use_mgm)
    //smooth_path_accumulation(left_image);
    smooth_path_accumulation_multithreaded(left_image);
  else
    //two_trip_path_accumulation(left_image);
    multi_thread_accumulation(left_image);
    
  vw_out(DebugMessage, "stereo") << "Accumulation finished, creating integer disparity image...\n";

  // Now that all the costs are calculated, fetch the best disparity for each pixel.
  // - This computes integer disparities.  Subpixel disparities are computed in CorrelationView.tcc
  return create_disparity_view();
}




// Perform standard SGM path accumulation using N threads.
void SemiGlobalMatcher::multi_thread_accumulation(ImageView<uint8> const& left_image) {

  //Timer timer_total("\tSGM Multi-Threaded Accumulation");

  int num_threads = vw_settings().default_num_threads();
  int height = m_num_output_rows;
  int width  = m_num_output_cols;

  ImageView<uint8> const* image_ptr = &left_image;
  Vector2i image_size(width, height);

  // Start up the thread pool
  FifoWorkQueue thread_pool(num_threads);
  vw_out() << "Starting thread pool with " << thread_pool.max_threads() << " threads.\n";
  
  // Initialize a number of line buffers equal to the number of threads
  OneLineBufferManager mem_buff_manager(num_threads, this);
  
  // Each direction of lines is handled seperately.  This adds some additional wait time
  //  as each direction finishes up but it allows the threads to simultaneously read/write
  //  the main accumulation buffer without any collisions.
  
  typedef boost::shared_ptr<PixelPassTask> TaskPtrType;
  
  // Add lines going down
  for (int i=0; i<width; ++i) { 
    Vector2i top_pixel(i, 0);
    PixelLineIterator line_from_top(top_pixel, PixelLineIterator::B, image_size);
    TaskPtrType task(new PixelPassTask(image_ptr, this, &mem_buff_manager, line_from_top));
    thread_pool.add_task(task);
  }
  thread_pool.join_all(); // Wait for all tasks to complete
  std::cout << "."; // TODO: Use a proper processing time bar!

  // Add lines going up
  for (int i=0; i<width; ++i) { 
    Vector2i bottom_pixel(i, height-1);
    PixelLineIterator line_from_bottom(bottom_pixel, PixelLineIterator::T, image_size);
    TaskPtrType task(new PixelPassTask(image_ptr, this, &mem_buff_manager, line_from_bottom));
    thread_pool.add_task(task);
  }
  thread_pool.join_all(); // Wait for all tasks to complete
  std::cout << ".";

  // Add lines from the left
  for (int i=0; i<height; ++i) { 
    Vector2i left_pixel(0, i);
    PixelLineIterator line_from_left (left_pixel, PixelLineIterator::R, image_size);
    TaskPtrType task(new PixelPassTask(image_ptr, this, &mem_buff_manager, line_from_left));
    thread_pool.add_task(task);
  }
  thread_pool.join_all(); // Wait for all tasks to complete
  std::cout << ".";

  // Add lines from the right
  for (int i=0; i<height; ++i) {
    Vector2i right_pixel(width-1, i);
    PixelLineIterator line_from_right(right_pixel, PixelLineIterator::L, image_size);
    TaskPtrType task(new PixelPassTask(image_ptr, this, &mem_buff_manager, line_from_right));
    thread_pool.add_task(task);
  }
  thread_pool.join_all(); // Wait for all tasks to complete
  std::cout << ".";

  // Add lines from the top left
  for (int i=0; i<width; ++i) {
    Vector2i top_pixel(i, 0);
    PixelLineIterator line_from_top_br(top_pixel, PixelLineIterator::BR, image_size);
    TaskPtrType task(new PixelPassTask(image_ptr, this, &mem_buff_manager, line_from_top_br));
    thread_pool.add_task(task);
  }
  for (int i=1; i<height; ++i) {
    Vector2i left_pixel(0, i);
    PixelLineIterator line_from_left_br(left_pixel, PixelLineIterator::BR, image_size);
    TaskPtrType task(new PixelPassTask(image_ptr, this, &mem_buff_manager, line_from_left_br));
    thread_pool.add_task(task);
  }
  thread_pool.join_all(); // Wait for all tasks to complete
  std::cout << ".";

  // Add lines from the top right
  for (int i=0; i<width; ++i) {
    Vector2i top_pixel(i, 0);
    PixelLineIterator line_from_top_bl(top_pixel, PixelLineIterator::BL, image_size);
    TaskPtrType task(new PixelPassTask(image_ptr, this, &mem_buff_manager, line_from_top_bl));
    thread_pool.add_task(task);
  }
  for (int i=1; i<height; ++i) {
    Vector2i right_pixel(width-1, i);
    PixelLineIterator line_from_right_bl(right_pixel, PixelLineIterator::BL, image_size);
    TaskPtrType task(new PixelPassTask(image_ptr, this, &mem_buff_manager, line_from_right_bl));
    thread_pool.add_task(task);
  }
  thread_pool.join_all(); // Wait for all tasks to complete
  std::cout << ".";

  // Add lines from the bottom left
  for (int i=0; i<width; ++i) {
    Vector2i bot_pixel(i, height-1);
    PixelLineIterator line_from_bot_tr(bot_pixel, PixelLineIterator::TR, image_size);
    TaskPtrType task(new PixelPassTask(image_ptr, this, &mem_buff_manager, line_from_bot_tr));
    thread_pool.add_task(task);
  }
  for (int i=0; i<height-1; ++i) {
    Vector2i left_pixel(0, i);
    PixelLineIterator line_from_left_tr(left_pixel, PixelLineIterator::TR, image_size);
    TaskPtrType task(new PixelPassTask(image_ptr, this, &mem_buff_manager, line_from_left_tr));
    thread_pool.add_task(task);
  }
  thread_pool.join_all(); // Wait for all tasks to complete
  std::cout << ".";
  
  // Add lines from the bottom right
  for (int i=0; i<width; ++i) {
    Vector2i bot_pixel(i, height-1);
    PixelLineIterator line_from_bot_tl(bot_pixel, PixelLineIterator::TL, image_size);
    TaskPtrType task(new PixelPassTask(image_ptr, this, &mem_buff_manager, line_from_bot_tl));
    thread_pool.add_task(task);
  }
  for (int i=0; i<height-1; ++i) {
    Vector2i right_pixel(width-1, i);
    PixelLineIterator line_from_right_tl(right_pixel, PixelLineIterator::TL, image_size);
    TaskPtrType task(new PixelPassTask(image_ptr, this, &mem_buff_manager, line_from_right_tl));
    thread_pool.add_task(task);
  }
  thread_pool.join_all(); // Wait for all tasks to complete
  std::cout << "Done with job batch.\n";

  // Finished!
  std::cout << "Finished multi-threaded accumulation!\n";
} // End function multi_thread_accumulation



// This version of the function requires four passes and is based on the paper:
// MGM: A Significantly More Global Matching for Stereovision
void SemiGlobalMatcher::smooth_path_accumulation_multithreaded(ImageView<uint8> const& left_image) {

  const int PATHS_PER_PASS = 1;
  const int MAX_USABLE_THREADS = 8;

  int num_threads = vw_settings().default_num_threads();

  // In order to use more than 8 threads at a time we need a more complicated MGM
  // implementation.  Currently each of the directions gets its own thread.
  if (num_threads > MAX_USABLE_THREADS) {
    num_threads = MAX_USABLE_THREADS;
    vw_out(WarningMessage) << "MGM stereo processing is currently limited to 8 parallel threads." << std::endl;
  }

  // Create objects to manage the temporary accumulation buffers that need to be used here.
  // - A separate copy instance is used for each direction to allow multithreading
  MultiAccumRowBuffer buff_manager_horizontal_left    (this, PATHS_PER_PASS, false);
  MultiAccumRowBuffer buff_manager_horizontal_right   (this, PATHS_PER_PASS, false);
  MultiAccumRowBuffer buff_manager_horizontal_topleft (this, PATHS_PER_PASS, false);
  MultiAccumRowBuffer buff_manager_horizontal_botright(this, PATHS_PER_PASS, false);
  
  MultiAccumRowBuffer buff_manager_vertical_top     (this, PATHS_PER_PASS, true);
  MultiAccumRowBuffer buff_manager_vertical_bot     (this, PATHS_PER_PASS, true);
  MultiAccumRowBuffer buff_manager_vertical_topright(this, PATHS_PER_PASS, true);
  MultiAccumRowBuffer buff_manager_vertical_botleft (this, PATHS_PER_PASS, true);
  
  // Set some of the buffers to the reverse direction
  buff_manager_horizontal_right.switch_trips();
  buff_manager_horizontal_botright.switch_trips();
  buff_manager_vertical_top.switch_trips();
  buff_manager_vertical_topright.switch_trips();

  // Start up thread pool
  FifoWorkQueue thread_pool(num_threads);
  vw_out() << "Starting thread pool with " << thread_pool.max_threads() << " threads.\n";

  // Load all eight required passes as task in to the thread pool   
  typedef boost::shared_ptr<SmoothPathAccumTask> TaskPtrType;
  TaskPtrType task_L (new SmoothPathAccumTask(&buff_manager_horizontal_left,     this, &left_image, SmoothPathAccumTask::L ));
  TaskPtrType task_R (new SmoothPathAccumTask(&buff_manager_horizontal_right,    this, &left_image, SmoothPathAccumTask::R ));
  TaskPtrType task_TL(new SmoothPathAccumTask(&buff_manager_horizontal_topleft,  this, &left_image, SmoothPathAccumTask::TL));
  TaskPtrType task_BR(new SmoothPathAccumTask(&buff_manager_horizontal_botright, this, &left_image, SmoothPathAccumTask::BR));
  TaskPtrType task_T (new SmoothPathAccumTask(&buff_manager_vertical_top,        this, &left_image, SmoothPathAccumTask::T ));
  TaskPtrType task_B (new SmoothPathAccumTask(&buff_manager_vertical_bot,        this, &left_image, SmoothPathAccumTask::B ));
  TaskPtrType task_TR(new SmoothPathAccumTask(&buff_manager_vertical_topright,   this, &left_image, SmoothPathAccumTask::TR));
  TaskPtrType task_BL(new SmoothPathAccumTask(&buff_manager_vertical_botleft,    this, &left_image, SmoothPathAccumTask::BL));
  
  thread_pool.add_task(task_L );
  thread_pool.add_task(task_R );
  thread_pool.add_task(task_TL);
  thread_pool.add_task(task_BR);
  thread_pool.add_task(task_T );
  thread_pool.add_task(task_B );
  thread_pool.add_task(task_TR);
  thread_pool.add_task(task_BL);
  
  thread_pool.join_all(); // Wait for all tasks to complete
  std::cout << "Done with multi threaded smooth accumulation.\n";  

} // End function smooth_path_accumulation_multithreaded



} // end namespace stereo
} // end namespace vw

