#include "BedFragments.h"

namespace BPCells {

BedFragments::BedFragments(const char *path, const char *comment_prefix) :
    path(path), f(NULL), next_chr_id(0), next_cell_id(0),
        comment(comment_prefix), last_start(0) {
    restart();
}

BedFragments::~BedFragments() {
    gzclose(f);
}

// Return the number of cells/chromosomes, or return -1 if this number is 
// not known ahead of time
int BedFragments::chrCount() const {return -1; };
int BedFragments::cellCount() const {return -1; };

const char* BedFragments::chrNames(uint32_t chr_id) const {
    if(chr_id >= chr_names.size()) return NULL;
    return chr_names[chr_id].c_str(); 
} 

const char* BedFragments::cellNames(uint32_t cell_id) const {
    if(cell_id >= cell_names.size()) return NULL;
    return cell_names[cell_id].c_str(); 
} 

uint32_t BedFragments::currentChr() const {
    return chr_lookup.at(current_chr); 
}

bool BedFragments::isSeekable() const {return false; };

void BedFragments::seek(uint32_t chr_id, uint32_t base) {
    throw std::invalid_argument("Cannot seek BedFragments");
}

const char* BedFragments::nextField(const char * c) {
    while (*c != '\0' && *c != '\t' && *c != '\n') {
        c++;
    }
    return c;
}

// Read the next line, returning false if we tried reading past the end of
// the file
bool BedFragments::read_line() {
    if (gzgets(f, &line_buf[0], line_buf.size()) == NULL) {
        if (eof) {
            line_buf[0] = '\0';
            return false;
        } else if (!gzeof(f)) {
            throw std::runtime_error("Error reading from gzfile");
        }
        eof = true;
    }

    return true;
}

// Parse the line in line_buf, returning the chromosome name as the actual
// return value, with output parameters for start, end, cell_id.
// Will assign a cell_id if it sees a new cell name.
// Returns empty string at eof
std::string BedFragments::parse_line(uint32_t &start, uint32_t &end, uint32_t &cell_id) {
    const char *cur_field, *next_field;

    cur_field = &line_buf[0];
    next_field = nextField(cur_field);
    std::string chr(cur_field, next_field - cur_field);

    if (next_field == cur_field) return chr;
    if (*next_field != '\t') throw std::runtime_error("Invalid TSV file");

    cur_field = next_field + 1;
    next_field = nextField(cur_field);
    if (cur_field == next_field || *next_field != '\t') throw std::runtime_error("Invalid TSV file");
    start = atoi(cur_field);

    cur_field = next_field + 1;
    next_field = nextField(cur_field);
    if (cur_field == next_field || *next_field != '\t') throw std::runtime_error("Invalid TSV file");
    end = atoi(cur_field);

    cur_field = next_field + 1;
    next_field = nextField(cur_field);
    auto cell_id_res = cell_id_lookup.emplace(
        std::string(cur_field, next_field-cur_field), 
        next_cell_id
    );
    if (cell_id_res.second) {
        cell_names.push_back(std::string(cur_field, next_field-cur_field));
        next_cell_id++;
    }
    cell_id = cell_id_res.first->second;
    
    return chr;
}

bool BedFragments::validInt(const char* c) {
    while (*c != '\0' && *c != '\t' && *c != '\n') {
        if (!isdigit(*c)) return false;
        c++;
    }
    return true;
}



void BedFragments::restart() {
    gzclose(f); // closing is fine if f is NULL
    f = gzopen(path.c_str(), "rb");
    if (f == NULL)
        throw std::invalid_argument("Could not open file");
    gzbuffer(f, 1 << 20);

    read_line();

    if (comment.size() == 0) return;

    // Loop through comment lines
    while(true) {
        if (line_buf[0] == '\0') break;
        bool matches_comment = true;
        for (int i = 0; i < comment.size(); i++) {
            if(line_buf[i] != comment[i]) {
                matches_comment=false;
                break;
            }
        }
        if(!matches_comment) break;
        read_line();
    }
    current_chr = "";
}

bool BedFragments::nextChr() {
    if (line_buf[0] == '\0' || line_buf[0] == '\n')
        return false;

    // Keep reading fragments until we get to the next chromosome
    uint32_t dummy_start, dummy_end, dummy_cell;
    while(true) {
        std::string chr = parse_line(dummy_start, dummy_end, dummy_cell);
        if (chr == "" || chr != current_chr) {
            current_chr = chr;
            break;
        }
        if (dummy_start < last_start) throw std::runtime_error("TSV not in sorted order by chr, start");
        last_start = dummy_start;
        if(!read_line()) return false;
    }

    auto chr_id_res = chr_lookup.emplace(
        current_chr, 
        next_chr_id
    );
    if (chr_id_res.second) {
        chr_names.push_back(current_chr);
        next_chr_id++;
    } else {
        throw std::runtime_error("TSV not in sorted order by chr, start");
    }
    last_start = 0;
    return true;
}

int32_t BedFragments::load(uint32_t count, FragmentArray &buffer) {
    std::string chr;

    for (size_t i = 0; i < count; i++) {
        // line_buf will contain the next line in file before start of loop 
        chr = parse_line(buffer.start[i], buffer.end[i], buffer.cell[i]);
        if (chr == "" || chr != current_chr) {
            return i;
        }
        if (buffer.start[i] < last_start) throw std::runtime_error("TSV not in sorted order by chr, start");
        last_start = buffer.start[i];

        if (!read_line()) return i;
    }

    return count;
};






BedFragmentsWriter::BedFragmentsWriter(const char *path, bool append_5th_column,
                    uint32_t buffer_size) : append_5th_column(append_5th_column) {
    
    std::string str_path(path);
    size_t extension_idx = str_path.rfind(".");
    if (extension_idx != std::string::npos &&
        str_path.substr(extension_idx) == ".gz") {
        f = gzopen(path, "wb1");
    } else {
        f = gzopen(path, "wT");
    }
    
    // Note default of large 1MB buffer to speed up reading
    gzbuffer(f, buffer_size);
}

BedFragmentsWriter::~BedFragmentsWriter() {
    gzclose(f);
}


bool BedFragmentsWriter::write(FragmentsIterator &fragments, void (*checkInterrupt)(void)) {
    uint32_t bytes_written;
    
    size_t total_fragments = 0;

    const char *output_format;
    if (append_5th_column) {
        output_format = "%s\t%d\t%d\t%s\t0\n";
    } else {
        output_format = "%s\t%d\t%d\t%s\n";
    }
    
    while (fragments.nextChr()) {
        const char* chr_name = fragments.chrNames(fragments.currentChr());
        while (fragments.nextFrag()) {
            bytes_written = gzprintf(f, output_format, 
                chr_name,
                fragments.start(),
                fragments.end(),
                fragments.cellNames(fragments.cell())
            );
            
            if (bytes_written <= 0) {
                return false;
            }

            if (checkInterrupt != NULL && total_fragments++ % 1024 == 0) checkInterrupt();
        }
    }
    return true;
};


} // end namespace BPCells