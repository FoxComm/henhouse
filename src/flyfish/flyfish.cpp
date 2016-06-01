#include <iostream>

#include "util/dbc.hpp"

#include <deque>
#include <string>
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/filesystem.hpp>
#include <exception>
#include <algorithm>
#include <fstream>
#include <chrono>
#include <cmath>

namespace bio = boost::iostreams;
namespace bf =  boost::filesystem;

namespace flyfish
{
    using time_type = std::uint64_t;
    using count_type = std::uint64_t;
    using change_type = std::int64_t;
    using offset_type = std::uint64_t;

    void open(bio::mapped_file& file, bf::path path, std::size_t new_size)
    {
        REQUIRE_GREATER(new_size, 0);

        if(bf::exists(path)) 
        {
            file.open(path);
        }
        else 
        {
            bio::mapped_file_params p;
            p.path = path.string();
            p.new_file_size = new_size;
            p.flags = bio::mapped_file::readwrite;

            file.open(p);
        }

        if(!file.is_open())
            throw std::runtime_error{"unable to mmap " + path.string()};
    }

    template<class T>
        T* open_as(bio::mapped_file& file, bf::path path, std::size_t new_size = sizeof(T))
        {
            REQUIRE_GREATER_EQUAL(new_size, sizeof(T));

            const auto size = std::max(new_size, sizeof(T));

            open(file, path, size);

            ENSURE_GREATER_EQUAL(file.size(), sizeof(T));
            return reinterpret_cast<T*>(file.data());
        }

    const std::size_t PAGE_SIZE = bio::mapped_file::alignment();
    const std::size_t DEFAULT_NEW_SIZE = PAGE_SIZE;

    template<class meta_t, class data_type>
    class mapped_vector
    {
        public:
            mapped_vector(){};
            mapped_vector(
                    const bf::path& meta_file, 
                    const bf::path& data_file, 
                    const std::size_t new_size = DEFAULT_NEW_SIZE) 
            {
                _data_file_path = data_file;
                _new_size = new_size;

                //open index metadata
                _metadata = open_as<meta_t>(_meta_file, meta_file);

                //open index data. New file size is new_size
                open(_data_file, data_file, new_size);
                _items = reinterpret_cast<data_type*>(_data_file.data());

                //compute max elements
                _max_items = _data_file.size() / sizeof(data_type);
                CHECK_LESS_EQUAL(_metadata->size, _max_items);

                ENSURE(_metadata != nullptr);
                ENSURE(_items != nullptr);
            }

            meta_t& meta() 
            {
                REQUIRE(_metadata);
                return *_metadata;
            }

            const meta_t& meta() const
            {
                REQUIRE(_metadata);
                return *_metadata;
            }

            std::uint64_t size() const 
            {
                REQUIRE(_metadata)
                ENSURE_LESS_EQUAL(_metadata->size, _max_items);
                return _metadata->size;
            }

            const data_type& operator[](size_t pos) const 
            {
                REQUIRE(_metadata); 
                REQUIRE(_items); 
                REQUIRE_LESS(pos, _metadata->size);

                return _items[pos];
            }

            data_type& operator[](size_t pos)
            {
                REQUIRE(_metadata); 
                REQUIRE(_items); 
                REQUIRE_LESS(pos, _metadata->size);

                return _items[pos];
            }

            void push_back(const data_type& v) 
            {
                REQUIRE(_metadata);
                const auto next_pos = _metadata->size;

                if(next_pos >= _max_items)
                    resize(_data_file.size() + _new_size);

                _items[next_pos] = v;
                _metadata->size++;
            }

            data_type* begin() 
            { 
                REQUIRE(_items);
                return _items;
            }

            data_type* end() 
            { 
                REQUIRE(_items);
                REQUIRE(_metadata);
                return _items + size();
            }

            data_type* begin() const
            { 
                REQUIRE(_items);
                return _items;
            }

            data_type* end() const
            { 
                REQUIRE(_items);
                REQUIRE(_metadata);
                return _items + size();
            }

            const data_type* cbegin() { return begin(); }
            const data_type* cend() { return end(); }
            const data_type* cbegin() const { return begin(); }
            const data_type* cend() const { return end(); }

            data_type& front()
            {
                REQUIRE(_items);
                return *_items;
            }

            const data_type& front() const
            {
                REQUIRE(_items);
                return *_items;
            }

            data_type& back()
            {
                REQUIRE(_items);
                REQUIRE(_metadata);
                REQUIRE_GREATER(_metadata->size, 0);
                return *(_items + (_metadata->size - 1));
            }

            const data_type& back() const
            {
                REQUIRE(_items);
                REQUIRE(_metadata);
                REQUIRE_GREATER(_metadata->size, 0);
                return *(_items + (_metadata->size - 1));
            }

        private:

            void resize(size_t new_size) 
            {
                _data_file.resize(new_size);
                _items = reinterpret_cast<data_type*>(_data_file.data());
                _max_items = _data_file.size() / sizeof(data_type);
            }


        protected:
            meta_t* _metadata = nullptr;
            data_type* _items = nullptr;
            std::size_t _max_items = 0;
            std::size_t _new_size = 0;
            bio::mapped_file _meta_file;
            bio::mapped_file _data_file;
            bf::path _data_file_path;
    };


    struct index_item
    {
        time_type time = 0;
        offset_type pos = 0;
    };


    struct index_metadata
    {
        std::uint64_t size = 0 ;
        std::uint64_t resolution = 0;
        std::uint64_t frame_size = 0;
    };

    struct data_metadata
    {
        std::uint64_t size = 0;
    };

    struct data_item
    {
        count_type value;
        count_type integral;
        count_type second_integral;
    };

    const std::size_t DATA_SIZE = PAGE_SIZE;
    const std::uint64_t DEFAULT_RESOLUTION = 100;
    const std::size_t INDEX_SIZE = PAGE_SIZE;
    const std::size_t FRAME_SIZE = DATA_SIZE / sizeof(data_item);

    struct pos_result
    {
        index_item range;
        offset_type offset;
        offset_type pos;
    };

    class index_type : public mapped_vector<index_metadata, index_item>
    {
        public:
            index_type() : mapped_vector{} {};
            index_type(const bf::path& meta_file, const bf::path& data_file) :
                mapped_vector{meta_file, data_file, INDEX_SIZE}
            {
                REQUIRE(_metadata);

                if(_metadata->resolution == 0) _metadata->resolution = DEFAULT_RESOLUTION;
                if(_metadata->frame_size == 0) _metadata->frame_size = FRAME_SIZE;
            }

            index_item find_range(time_type t) const 
            {
                REQUIRE(_metadata);
                REQUIRE(_items);

                auto r = std::upper_bound(cbegin(), cend(), t, 
                        [](time_type l, const auto& r) { return l < r.time;});

                return r != cbegin() ? *(r - 1) : front();
            }

            pos_result find_pos(time_type t, const index_item range) const
            {
                REQUIRE(_metadata);

                t = std::max(t, range.time);
                const auto offset_time = t - range.time;
                const auto offset = offset_time / _metadata->resolution;
                return pos_result{ range, offset, range.pos + offset};
            }

            pos_result find_pos(time_type t) const
            {
                if(size() == 0) return pos_result {index_item { t, 0}, 0, 0};
                const auto range = find_range(t);
                return find_pos(t, range);
            }
    };

    using key_t = std::string;


    using data_type = mapped_vector<data_metadata, data_item>;

    void propogate(data_item prev, data_item& current)
    {
        current.integral = prev.integral + current.value;
        current.second_integral = prev.second_integral + (current.value * current.value);
    }

    void update_current(data_item prev, data_item& current, count_type c)
    {
        current.value += c;
        propogate(prev, current);
    }

    struct diff_result
    {
        count_type sum;
        count_type mean;
        count_type variance;
        change_type change;
        count_type size;
    };

    diff_result diff_buckets(data_item a, data_item b, std::size_t n)
    {
        REQUIRE_GREATER_EQUAL(b.integral, a.integral);
        REQUIRE_GREATER_EQUAL(b.second_integral, a.second_integral);

        const auto sum = b.integral - a.integral;
        const auto second_sum = b.second_integral - a.second_integral;
        const auto mean = n > 0 ? sum / n : a.value;
        const auto mean_squared = mean * mean;
        const auto second_mean = n > 0 ? second_sum / n : (a.value * a.value);
        const auto variance = second_mean - mean_squared;
        const change_type change = b.value > a.value ? (b.value - a.value) : -(a.value - b.value);
        return diff_result 
        { 
            sum, 
            mean, 
            variance,
            change,
            n
        };
    }

    struct timeline
    {
        key_t key;
        index_type index;
        data_type data;

        bool put(time_type t, count_type c)
        {
            if(index.size() > 0)
            {
                const auto& last_range = index.back();

                //don't add if time is before last range
                if(t >= last_range.time) 
                {
                    //get last position only because we want to keep 
                    //a specific performance profile. This is a deliberate limitation.
                    auto r = index.find_pos(t, last_range);

                    //bucket is current or in the past, no need to index.
                    if(r.pos < data.size())
                    {
                        const auto prev = r.pos > 0 ? data[r.pos - 1] : data_item {0, 0, 0};
                        update_current(prev, data[r.pos], c);
                        for(auto p = r.pos + 1; p < data.size(); p++)
                            propogate(data[p-1], data[p]);
                    }
                    //if we move beyond end, append data 
                    else
                    {
                        r.pos = data.size();
                        const auto prev = data[r.pos - 1];
                        data_item current = { c, 0};
                        propogate(prev, current);
                        data.push_back(current);

                        if(r.offset < index.meta().frame_size) return true;

                        //index only if the offset is same or bigger than the frame size.
                        const auto resolution = index.meta().resolution;
                        const auto aliased_time = r.range.time + (r.offset * resolution);
                        index_item index_entry = {aliased_time, r.pos};
                        index.push_back(index_entry);
                    }
                }
                else return false;
            }
            else
            {
                CHECK_EQUAL(data.size(), 0);

                data_item v{ c, 0};
                data.push_back(v);

                index_item i = {t, 0};
                index.push_back(i);
            }

            return true;
        }

        const data_item& get(time_t t) const  
        {
            const auto r = index.find_pos(t);
            REQUIRE_RANGE(r.pos, 0, data.size());
            return data[r.pos];
        }

        diff_result diff(time_type a, time_type b) 
        {
            if(a > b) std::swap(a,b);
            if(data.size() == 0) return diff_result{ 0, 0, 0};

            auto ar = index.find_pos(a);
            auto br = index.find_pos(b);

            ar.pos = std::max<offset_type>(0, ar.pos);
            ar.pos = std::min<offset_type>(data.size() - 1, ar.pos);

            br.pos = std::max<offset_type>(0, br.pos);
            br.pos = std::min<offset_type>(data.size() - 1, br.pos);

            const auto& ad = data[ar.pos]; 
            const auto& bd = data[br.pos]; 

            CHECK_GREATER_EQUAL(b, a);
            const auto n = (b - a) / index.meta().resolution;

            return diff_buckets(ad, bd, n);
        }
    };

    timeline from_directory(const std::string& path) 
    {
        if(!bf::is_directory(path))
           throw std::runtime_error{"path " + path + " is not a directory"}; 
    
        bf::path root = path;

        timeline t;
        t.key = root.filename().string();

        bf::path idx_meta = root / "im";
        bf::path idx_data = root / "id";
        t.index = index_type{idx_meta, idx_data};
        
        bf::path cmeta = root / "m";
        bf::path cdata = root / "d";

        t.data = data_type{cmeta, cdata, DATA_SIZE};
        
        return t;
    }
}

int main(int argc, char** argv)
try
{
    auto db = flyfish::from_directory("./tmp");

    size_t tm = 0;

    //const size_t TOTAL = 1000000000;
    const int TOTAL = 100000000;
    const size_t TIME_INC = 10;
    const int CHANGE = 2;

    std::cout << std::fixed;
    if(db.data.size() == 0)
    {
        auto start = std::chrono::high_resolution_clock::now();
        for(int a = 0; a < TOTAL; a++)
        {
            tm += TIME_INC;
            auto v = a % 3 ? 1 : 2;
            db.put(tm, v);
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end-start).count();
        auto sec = duration / 1000000000.0;
        std::cout <<  sec << " seconds" << std::endl;
        std::cout << (TOTAL / sec) << " puts per second" << std::endl;
    }
    else
    {
        tm = TOTAL * TIME_INC;
    }

    auto start = std::chrono::high_resolution_clock::now();

    auto d = db.diff(tm/2, tm/4);

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end-start).count();
    auto sec = duration / 1000000000.0;

    std::cout << "query time: " << sec << "s" << std::endl;

    std::cout << "diff sum: " << d.sum << std::endl;
    std::cout << "diff avg: " << d.mean << std::endl;
    std::cout << "diff variance: " << d.variance << std::endl;
    std::cout << "diff change: " << d.change << std::endl;
    std::cout << "diff size: " << d.size << std::endl;

    std::cout << "total puts: " << TOTAL << std::endl;
    std::cout << "ranges: " << db.index.size() << std::endl;
    std::cout << "buckets: " << db.data.size() << std::endl;


    return 0;
}
catch(std::exception& e) 
{
    std::cerr << "Error! " << e.what() << std::endl;
    return 1;
}
