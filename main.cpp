#include <iostream>
#include <map>
#include <vector>
#include <thread>
#include <fstream>
#include <boost/filesystem.hpp>
#include <boost/locale.hpp>
#include <boost/algorithm/string.hpp>
#include <sstream>
#include <numeric>
#include <queue>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <limits>
#include <chrono>
#include "tbb/concurrent_queue.h"
#include "tbb/task_group.h"
#include <zip.h>



inline std::chrono::steady_clock::time_point get_current_time_fenced() {
    assert(std::chrono::steady_clock::is_steady &&
                   "Timer should be steady (monotonic).");
    std::atomic_thread_fence(std::memory_order_seq_cst);
    auto res_time = std::chrono::steady_clock::now();
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return res_time;
}

template<class D>
inline long long to_us(const D& d)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
}

struct configuration_t
{
    std::string indir, out_by_a, out_by_n;
    size_t indexing_threads, merging_threads;
};

struct recursive_directory_range
{
    typedef boost::filesystem::recursive_directory_iterator iterator;
    recursive_directory_range(boost::filesystem::path p) : p_(p) {}

    iterator begin() {
        return boost::filesystem::recursive_directory_iterator(p_);
    }
    iterator end() {
        return boost::filesystem::recursive_directory_iterator();
    }

    boost::filesystem::path p_;
};


class main_task {
private:
    std::string path_entry;   //directory where scannig starts

    size_t indexing_threads_count;
    size_t merging_threads_count;


    tbb::atomic<int> scanning_threads_working;
    tbb::atomic<int> reading_threads_working;
    tbb::atomic<int> indexing_threads_working;
    tbb::atomic<int> merging_threads_working;
    tbb::atomic<int> queue_indexes_size {0};


    std::string out_by_a;
    std::string out_by_n;

    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;


    tbb::concurrent_queue<std::string> queue_filepath;
    tbb::concurrent_queue<std::string> queue_texts;
    tbb::concurrent_queue<std::map<std::string, int>> queue_indexes;


    tbb::task_group tasks;

    void read_configuration(const std::string & conf_file_name);

    void scanning_routine();
    void reading_into_memory_routine();
    void indexing_routine();
    void merging_routine();



public:

    main_task(const std::string & a);
    void do_main_routine();
    void writing_results();
    std::chrono::steady_clock::duration result_time(){
        return end_time - start_time;
    }
};


void main_task::read_configuration(const std::string & conf_file_name) {
    std::ifstream cf(conf_file_name);
    if(!cf.is_open()) {
        std::cerr << "Failed to open configuration file " << conf_file_name << std::endl;
        return;
    }

    std::ios::fmtflags flags( cf.flags() );
    cf.exceptions(std::ifstream::failbit);

    configuration_t res;
    std::string temp;

    try {
        cf >> res.indir;
        getline(cf, temp);
        cf >> res.out_by_a;
        getline(cf, temp);
        cf >> res.out_by_n;
        getline(cf, temp);
        cf >> res.indexing_threads;
        getline(cf, temp);
        cf >> res.merging_threads;
        getline(cf, temp);

    }catch(std::ios_base::failure &fail)
    {
        cf.flags( flags );
        throw;
    }
    cf.flags( flags );
    if( res.indexing_threads < 1 || res.merging_threads < 1) {
        throw std::runtime_error("The number of threads should be at least 1");
    }

    path_entry = res.indir;
    indexing_threads_count = res.indexing_threads;
    merging_threads_count = res.merging_threads;
    out_by_a = res.out_by_a;
    out_by_n = res.out_by_n;
}


void main_task::scanning_routine(){
    scanning_threads_working ++;
    for (auto &it : recursive_directory_range(path_entry))
    {
        if(is_regular_file(it)){
            std :: string extension = boost::filesystem::extension(it);
            if ((extension == ".txt") || (extension == ".zip") || (extension == ".TXT") || (extension == ".ZIP")) {
                queue_filepath.push((it.path()).string());
            }
        }
    }
    scanning_threads_working--;
}

void main_task::reading_into_memory_routine(){
    reading_threads_working ++;


    std::string file_name;
    std::string block;
    std::stringstream ss;

    while(scanning_threads_working || !queue_filepath.empty()) {
        if(queue_filepath.try_pop(file_name)) {
            auto file_extension = boost::filesystem::extension(file_name);

            if (file_extension != ".txt" && file_extension != ".TXT") {
                struct zip *zip_file; // zip file descriptor
                struct zip_file *file_in_zip; // file decriptor inside archive
                struct zip_stat file_info;
                int err; // error code
                int files_total; // count of files in archive
                int file_number;
                int r;
                char buffer[10000];

                zip_file = zip_open(file_name.c_str(), 0, &err);
                if (!zip_file) {
                    std::cout << "Error: can't open file " << file_name.c_str() << std::endl;
                    continue;
                };


                files_total = zip_get_num_files(zip_file);

                for (int file_number = 0; file_number < files_total; file_number++) {
                    zip_stat_index(zip_file, file_number, 0, &file_info);

                    if(boost::filesystem::extension(file_info.name) != ".txt" && boost::filesystem::extension(file_info.name) != ".TXT")
                        continue;


                    file_in_zip = zip_fopen_index(zip_file, file_number, 0);

                    if(file_in_zip) {
                        std::string file_content = "";

                        while ( (r = zip_fread(file_in_zip, buffer, sizeof(buffer))) > 0) {
                            file_content.append(buffer, r);
                        };
                        zip_fclose(file_in_zip);
                        queue_texts.push(file_content);
                    } else {
                        std::cout << "     Error: can't open file in zip " << file_info.name << std::endl;
                    };
                };

                zip_close(zip_file);
            } else if (file_extension == ".txt" || file_extension == ".TXT"){

                std::ifstream file (file_name); //method of reading the file from cms link
                auto const start_pos = file.tellg();
                file.ignore(std::numeric_limits<std::streamsize>::max());
                auto const char_count = file.gcount();
                file.seekg(start_pos);
                auto s = std::vector<char>((unsigned long)char_count);
                (file).read(&s[0], s.size());
                file.close();

                queue_texts.push(std::string(s.begin(), s.end()));
            }
        }
    }
    reading_threads_working --;
}
void main_task::indexing_routine()
{
    indexing_threads_working++;
    boost::locale::generator gen;
    std::locale loc=gen("");
    std::locale::global(loc);
    std::cout.imbue(loc);
    while(reading_threads_working || !queue_texts.empty())
    {
        std::string txt;
        if(queue_texts.try_pop(std::ref(txt))) {
            std::vector <std::string> vect_words;
            std::map <std::string, int> map_words;

            boost::locale::boundary::ssegment_index map(boost::locale::boundary::word, txt.begin(), txt.end());
            map.rule(boost::locale::boundary::word_any);
            for (boost::locale::boundary::ssegment_index::iterator it = map.begin(), e = map.end(); it != e; ++it) {
                std::string str = *it;
                vect_words.push_back(boost::locale::fold_case(boost::locale::normalize(str)));

            }
            for (int i = 0; i < vect_words.size(); ++i)
            {
                ++map_words[vect_words[i]];
            }
            queue_indexes.push(map_words);

            queue_indexes_size ++;

            vect_words.clear();
            map_words.clear();

        }
    }

    indexing_threads_working--;

}

void main_task::merging_routine() {
    merging_threads_working ++;

    std :: map <std :: string, int> map1, map2, result_map;

    while(true) {
        if (queue_indexes.try_pop(map1)){
            if(queue_indexes.try_pop(map2)) {
                for (auto &word : map2)
                    map1[word.first] += word.second;
                    queue_indexes.push(map1);
            } else {
               queue_indexes.push(map1);
               if (!indexing_threads_working || merging_threads_working > 1)
                   break;
            }
        }
    }
    queue_indexes_size --;

    merging_threads_working --;
}

void PrintInFile(std :: fstream & file, const std::vector <std::pair < std::string, int>> & vect){
    if(!file.is_open()){
        std :: cout << "Problem with file" << std :: endl;
        return;
    }
    for (int i = 0; i < vect.size(); i ++) {
        file << vect[i].first << " : " << vect[i].second << std::endl;
    }
}

void main_task::writing_results() {


    std::vector <std::pair<std::string, int>> v;
    std::map <std::string, int> result;

    std :: fstream fs_out_by_a (out_by_a);
    std :: fstream fs_out_by_n (out_by_n);


    std::map <std::string, int> temp;
    if(queue_indexes.unsafe_size() == 1){
        queue_indexes.try_pop(result);
        copy(result.begin(), result.end(), back_inserter(v));

        PrintInFile(fs_out_by_a, v);
        sort(v.begin(), v.end(), [] (const std::pair <std::string, int> & a, const std::pair <std::string, int> & b){
            return (a.second > b.second);
        });
        PrintInFile(fs_out_by_n, v);
    } else {
        std::cout << "You are looser" << std::endl;
        return;
    }
    std::cout << to_us(result_time() / 1000000 ) << " s" << std::endl;
}

main_task::main_task(const std::string & conf_file_name)
{
    read_configuration(conf_file_name);
}

void main_task::do_main_routine()
{
    start_time = get_current_time_fenced();


    tasks.run([&]{scanning_routine();});
    tasks.run([&]{reading_into_memory_routine();});


    for(int i = 0; i <indexing_threads_count; i++)
        tasks.run([&]{indexing_routine();});

    for(int i = 0; i <merging_threads_count; i++)
        tasks.run([&]{merging_routine();});

    tasks.wait();

    end_time = get_current_time_fenced();

}



int main(int argc, char* argv[])
{

    std::string conf_file_name ("conf.txt");
    if(argc == 2)
        conf_file_name = argv[1];
    if(argc > 2) {
        std::cerr << "Too many arguments. Usage: \n"
                     "<program>\n"
                     "or\n"
                     "<program> <config-filename>\n" << std::endl;
        return 1;
    }

    main_task tsk(conf_file_name);

    tsk.do_main_routine();
    tsk.writing_results();
}
