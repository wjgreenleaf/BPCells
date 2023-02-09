#include "ImportMatrixHDF5.h"

namespace BPCells {

// Reader interfaces for 10x and AnnData matrices

StoredMatrix<uint32_t>
open10xFeatureMatrix(std::string file, uint32_t buffer_size, uint32_t read_size) {
    HighFive::File f(file, HighFive::File::ReadWrite);

    // Most up-to-date matrix format
    if (f.exist("matrix")) {
        H5ReaderBuilder rb(file, "matrix", buffer_size, read_size);
        uint32_t rows = rb.openUIntReader("shape").read_one();
        return StoredMatrix(
            rb.openULongReader("indices").convert<uint32_t>(),
            rb.openUIntReader("data"),
            rb.openULongReader("indptr").convert<uint32_t>(),
            rows,
            rb.openStringReader("features/id"),
            rb.openStringReader("barcodes")
        );
    }

    // Older-style 10x matrix format
    std::vector<std::string> genomes = f.listObjectNames();
    if (genomes.size() != 1) {
        throw std::runtime_error(
            "Loading multi-genome matrices from old-style 10x hdf5 files is unsupported"
        );
    }

    H5ReaderBuilder rb(file, genomes[0], buffer_size, read_size);
    uint32_t rows = rb.openUIntReader("shape").read_one();
    return StoredMatrix(
        rb.openULongReader("indices").convert<uint32_t>(),
        rb.openUIntReader("data"),
        rb.openULongReader("indptr").convert<uint32_t>(),
        rows,
        rb.openStringReader("genes"),
        rb.openStringReader("barcodes")
    );
}

StoredMatrixWriter<uint32_t> create10xFeatureMatrix(
    std::string file_path,
    const StringReader &barcodes,
    const StringReader &feature_ids,
    const StringReader &feature_names,
    const StringReader &feature_types,
    const std::map<std::string, std::unique_ptr<StringReader>> &feature_metadata,
    uint32_t buffer_size,
    uint32_t chunk_size
) {
    H5WriterBuilder wb(file_path, "matrix", buffer_size, chunk_size);

    wb.createStringWriter("barcodes")->write(barcodes);
    wb.createStringWriter("features/id")->write(feature_ids);
    wb.createStringWriter("features/name")->write(feature_names);
    wb.createStringWriter("features/feature_type")->write(feature_types);
    std::vector<std::string> tag_keys;
    for (const auto &[key, value] : feature_metadata) {
        wb.createStringWriter(std::string("features/") + key)->write(*value);
        tag_keys.push_back(key);
    }
    wb.createStringWriter("features/_all_tag_keys")->write(VecStringReader(tag_keys));

    return StoredMatrixWriter(
        wb.createULongWriter("indices").convert<uint32_t>(),
        wb.createUIntWriter("data"),
        wb.createULongWriter("indptr").convert<uint32_t>(),
        wb.createUIntWriter("shape"),
        std::make_unique<NullStringWriter>(),
        std::make_unique<NullStringWriter>(),
        std::make_unique<NullStringWriter>()
    );
}

// Read AnnData sparse matrix, with an implicit transpose to CSC format for
// any data stored in CSR format
StoredMatrix<float>
openAnnDataMatrix(std::string file, std::string group, uint32_t buffer_size, uint32_t read_size) {
    H5ReaderBuilder rb(file, group, buffer_size, read_size);

    HighFive::SilenceHDF5 s;
    HighFive::Group &g = rb.getGroup();
    std::string encoding;
    std::vector<uint32_t> dims;

    HighFive::Group root = g.getFile().getGroup("/");
    std::unique_ptr<StringReader> col_names, row_names;

    if (g.hasAttribute("h5sparse_format")) {
        // Support for legacy format
        g.getAttribute("h5sparse_format").read(encoding);
        g.getAttribute("h5sparse_shape").read(dims);
        encoding += "_matrix";

        auto dt = root.getDataSet("var").getDataType();

        std::vector<std::string> row_name_data, col_name_data;
        readMember(root.getDataSet("var"), "index", col_name_data);
        readMember(root.getDataSet("obs"), "index", row_name_data);
        col_names = std::make_unique<VecStringReader>(col_name_data);
        row_names = std::make_unique<VecStringReader>(row_name_data);
    } else if (g.hasAttribute("encoding-type")) {
        g.getAttribute("encoding-type").read(encoding);
        g.getAttribute("shape").read(dims);

        std::string row_ids, col_ids;
        root.getGroup("var").getAttribute("_index").read(col_ids);
        root.getGroup("obs").getAttribute("_index").read(row_ids);
        col_ids = "var/" + col_ids;
        row_ids = "obs/" + row_ids;
        col_names = std::make_unique<H5StringReader>(root, col_ids);
        row_names = std::make_unique<H5StringReader>(root, row_ids);
    } else {
        throw std::runtime_error(
            "h5ad could not be read - missing attribute 'encoding-type' on sparse matrix"
        );
    }

    uint32_t rows;
    if (encoding == "csr_matrix") {
        rows = dims[1];
        std::swap(row_names, col_names);
    } else if (encoding == "csc_matrix") {
        rows = dims[0];
    } else throw std::runtime_error("Unsupported matrix encoding: " + encoding);

    return StoredMatrix<float>(
        rb.openUIntReader("indices"),
        rb.openFloatReader("data"),
        rb.openUIntReader("indptr"),
        rows,
        std::move(row_names),
        std::move(col_names)
    );
}

bool isRowOrientedAnnDataMatrix(std::string file, std::string group) {
    H5ReaderBuilder rb(file, group, 1024, 1024);

    HighFive::SilenceHDF5 s;
    HighFive::Group &g = rb.getGroup();
    std::string encoding;

    if (g.hasAttribute("h5sparse_format")) {
        // Support for legacy format
        g.getAttribute("h5sparse_format").read(encoding);
        encoding += "_matrix";
    } else if (g.hasAttribute("encoding-type")) {
        g.getAttribute("encoding-type").read(encoding);
    } else {
        throw std::runtime_error(
            "h5ad could not be read - missing attribute 'encoding-type' on sparse matrix"
        );
    }

    if (encoding == "csr_matrix") return true;
    else if (encoding == "csc_matrix") return false;
    else throw std::runtime_error("Unsupported matrix encoding: " + encoding);
}

template <typename T>
void readMember(HighFive::DataSet &&dataset, std::string name, std::vector<T> &out) {
    auto dims = dataset.getDimensions();
    if (dims.size() != 1) {
        std::ostringstream ss;
        ss << "readMember: dims must be 1, found " << dims.size();
        throw std::runtime_error(ss.str());
    }
    HighFive::CompoundType base_type = dataset.getDataType();
    HighFive::DataType field_type = HighFive::create_datatype<T>();
    bool found = false;
    // Error checking that the type exists
    for (auto &t : base_type.getMembers()) {
        if (t.name == name) {
            if (t.base_type.getClass() == field_type.getClass()) {
                found = true;
                break;
            }
            std::ostringstream ss;
            ss << "Type of member \"" << name << "\" in file (" << t.base_type.string()
               << ") does not match class of requested type (" << field_type.string() << ")";
            throw HighFive::DataTypeException(ss.str());
        }
    }
    if (!found) {
        std::ostringstream ss;
        ss << "Member \"" << name << "\" not found in compound data type";
        throw HighFive::DataTypeException(ss.str());
    }
    HighFive::CompoundType subtype({{name.c_str(), field_type}});

    out.resize(dims[0]);
    // Use a data-converter like the fancy read function (makes string conversion etc. work)
    auto r = HighFive::details::data_converter::get_reader<std::vector<T>>(dims, out);
    dataset.read(r.get_pointer(), subtype);
    // re-arrange results
    r.unserialize();
}

} // end namespace BPCells
