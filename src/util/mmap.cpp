#include "util/mmap.hpp" 

namespace fs = boost::filesystem;

namespace henhouse::util
{
    bool open(bio::mapped_file& file, fs::path path, std::size_t new_size)
    {
        REQUIRE_GREATER(new_size, 0);

        bool created = false;

        if(fs::exists(path)) 
        {
            file.open(path);
        }
        else 
        {
            bio::mapped_file_params p;
            p.path = path.string();
            p.new_file_size = std::max(new_size, PAGE_SIZE);
            p.flags = bio::mapped_file::readwrite;

            file.open(p);
            created = true;
        }

        if(!file.is_open())
            throw std::runtime_error{"unable to mmap " + path.string()};

        return created;
    }
}
