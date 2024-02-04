import os
from manpage import ManPage
from extract_utils import extract_tcl_command, extract_description
from extract_utils import extract_tcl_code, extract_arguments
from extract_utils import extract_tables, parse_switch

# Undocumented manpages.
# sta: documentation is hosted elsewhere. (not currently in RTD also.)
# odb: documentation is hosted on doxygen. 

tools = ["ant", "cts", "dbSta", "dft", "dpl", "dpo", "drt",\
        "dst", "fin", "gpl", "grt", "gui", "ifp", "mpl",\
        "mpl2", "odb", "pad", "par", "pdn", "ppl", "psm",\
        "rcx", "rmp", "rsz", "sta", "stt", "tap", "upf", "utl"]

# Process man2 (except odb and sta)
DEST_DIR2 = SRC_DIR = "./md/man2"
exclude2 = ["odb", "sta"]
docs2 = [f"{SRC_DIR}/{tool}.md" for tool in tools if tool not in exclude2]

# Process man3 (add extra path for ORD messages)
SRC_DIR = "../src"
DEST_DIR3 = "./md/man3"
exclude = ["sta"] #sta excluded because its format is different, and no severity level.
docs3 = [f"{SRC_DIR}/{tool}/messages.txt" for tool in tools if tool not in exclude]
docs3.append("../messages.txt")

def man2(path=DEST_DIR2):
    for doc in docs2:
        if not os.path.exists(doc):
            print(f"{doc} doesn't exist. Continuing")
            continue
        text = open(doc).read()

        # new function names (reading tcl synopsis + convert gui:: to gui_)
        func_names = extract_tcl_command(text)
        func_names = ["_".join(s.lower().split()) for s in func_names]
        func_names = [s.replace("::", "_") for s in func_names]

        # function description
        func_descs = extract_description(text)

        # synopsis content
        func_synopsis = extract_tcl_code(text)

        # arguments
        func_options, func_args = extract_arguments(text)

        print(f'{os.path.basename(doc)}')
        print(f'''Names: {len(func_names)},\
        Desc: {len(func_descs)},\
        Syn: {len(func_synopsis)},\
        Options: {len(func_options)},\
        Args: {len(func_args)}''')

        for func_id in range(len(func_synopsis)):
            manpage = ManPage()
            manpage.name = func_names[func_id]
            manpage.desc = func_descs[func_id]
            manpage.synopsis = func_synopsis[func_id]
            if func_options[func_id]:
                # convert it to dict 
                # TODO change this into a function. Or subsume under option/args parsing.
                switches_dict = {}
                for line in func_options[func_id]:
                    key, val = parse_switch(line)
                    switches_dict[key] = val
                manpage.switches = switches_dict
            
            if func_args[func_id]:
                # convert it to dict
                args_dict = {}
                for line in func_args[func_id]:
                    key, val = parse_switch(line)
                    args_dict[key] = val
                manpage.args = args_dict

            manpage.write_roff_file(path)

def man3(path=DEST_DIR3):
    for doc in docs3:
        _info, _warn, _error = 0, 0, 0
        print(f"Processing {doc}")
        if not os.path.exists(doc):
            print(f"{doc} doesn't exist. Continuing")
            continue
        with open(doc, 'r') as f:
            for line in f:
                parts = line.split()
                module, num, message, level = parts[0], parts[1],\
                                " ".join(parts[3:-2]), parts[-2]
                manpage = ManPage()
                manpage.name = f"{module}-{num}"
                if "with-total" in manpage.name: print(parts); exit()
                manpage.synopsis = "N/A."
                manpage.desc = f"Type: {level}\n\n{message}"
                manpage.write_roff_file(path)

                # tabulate counts
                if level == 'INFO': _info += 1
                elif level == 'WARN': _warn += 1
                elif level == 'ERROR': _error += 1
            print(f"Info: {_info}, Warn: {_warn}, Error: {_error}")


if __name__ == "__main__":
    man2()
    man3()