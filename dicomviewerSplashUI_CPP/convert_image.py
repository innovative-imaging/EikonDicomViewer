import os
import sys

def png_to_cpp_array(png_file_path, output_header_path):
    """Convert PNG file to C++ byte array header file"""
    
    if not os.path.exists(png_file_path):
        print(f"Error: PNG file not found: {png_file_path}")
        return False
    
    # Read the PNG file
    with open(png_file_path, 'rb') as f:
        png_data = f.read()
    
    # Generate the header file
    header_content = f"""#ifndef SPLASH_IMAGE_DATA_H
#define SPLASH_IMAGE_DATA_H

// Auto-generated from {os.path.basename(png_file_path)}
// File size: {len(png_data)} bytes

const unsigned char SPLASH_IMAGE_DATA[] = {{
"""
    
    # Convert bytes to C++ array format
    for i, byte in enumerate(png_data):
        if i % 16 == 0:
            header_content += "\n    "
        header_content += f"0x{byte:02x},"
        if i < len(png_data) - 1 and i % 16 != 15:
            header_content += " "
    
    header_content += f"""
}};

const unsigned int SPLASH_IMAGE_SIZE = {len(png_data)};

#endif // SPLASH_IMAGE_DATA_H
"""
    
    # Write the header file
    try:
        with open(output_header_path, 'w') as f:
            f.write(header_content)
        print(f"Successfully generated {output_header_path}")
        print(f"Image size: {len(png_data)} bytes")
        return True
    except Exception as e:
        print(f"Error writing header file: {e}")
        return False

if __name__ == "__main__":
    # Convert CompanySplashScreen.png to header file
    png_path = "../CompanySplashScreen.png"
    header_path = "splash_image_data.h"
    
    if png_to_cpp_array(png_path, header_path):
        print("Image conversion completed successfully!")
    else:
        print("Image conversion failed!")
        sys.exit(1)