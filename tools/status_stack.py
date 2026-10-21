"""Direct status frames retained across capture, count/fill and checked digits."""
def status_paths(frames,model,label,prefix,reserve,compat=False,capture=True):
    try:
        from .json_numeric_stack import numeric_tails
    except ImportError:
        from json_numeric_stack import numeric_tails
    paths={}
    captures=[model.lower()+'_status_capture']+[n for n in ('bemfa_status_capture','bemfa_config_get','runtime_metrics_capture','feature_fan_snapshot','watchdog_status_capture','a1_filter_get','ph_http_snapshot','tc1_status_begin','tc1_ota_snapshot','reset_name','device_status_text') if n in frames]
    if capture:paths[label+' capture']=(prefix+captures,reserve)
    wire=prefix+(['device_status_encode'] if compat else [])+['device_status_wire']
    schema=wire+[n for n in ('status_wire_visit','device_status_visit') if n in frames]+['encode_'+model.lower()]
    # Key escaping and value escaping are sequential. Summing these tiny tails
    # conservatively also covers prefix's nested quote/byte helper.
    for kind in ('string','int','bool','null','begin','end'):
        tail=[n for n in ('emit_'+kind,'emit_prefix','emit_quoted','emit_bytes') if n in frames]
        paths[label+' wire '+kind]=(schema+tail,reserve)
    if model!='TC1':
        for branch,tail in numeric_tails(frames,'format').items():
            paths[label+' wire numeric '+branch]=(schema+[n for n in ('runtime_fields','emit_double') if n in frames]+tail,reserve)
    for api in ('json_heap_malloc','json_heap_calloc'):
        for tail in ('permit','created'):
            paths[label+' wire '+api+' '+tail]=(wire+[api,'json_heap.'+tail,'json_heap_failure'],reserve)
    if compat:
        # Compatibility graph reconstruction happens AFTER the independent wire
        # encoder returns, so it does not retain schema or numeric-format frames.
        for branch,tail in numeric_tails(frames,'scan').items():
            paths[label+' compatibility parse '+branch]=(prefix+['device_status_encode','json_tokener_parse_ex']+tail,reserve)
    return paths
