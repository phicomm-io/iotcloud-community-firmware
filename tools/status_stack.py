"""Actual MessagePack status capture and encoding are sequential stack tails.

Only compiler-inlined private helpers may lack a separate .su frame. If nm
still emits one of those helpers, absence of its stack record is an error.
Public entry points and explicitly noinline schema visitors are mandatory.
"""

def emitted(frames, names, symbols=None):
    result=[]
    for name in names:
        if symbols is not None and name in symbols and name not in frames:
            raise ValueError('Emitted helper missing actual .su frame: '+name)
        if name in frames: result.append(name)
    return result


def writer_paths(frames, label, prefix, reserve, symbols=None, kinds=None, finalize=True):
    """Conservative direct writer tails; allocator reserve is not JSON scratch."""
    paths={}
    for kind, api in [('string',['phw_text','phw_text_n']),('integer',['phw_i32','phw_int']),
                      ('unsigned',['phw_u32','phw_uint']),('number',['phw_number']),
                      ('bool',['phw_bool']),('null',['phw_null']),('begin',['phw_begin'])]:
        if kinds is not None and kind not in kinds:continue
        # The outer public API is always emitted in the no-LTO product build.
        # Additional public wrappers are also charged if present (.su includes
        # eliminated functions); private inlining resides in its caller frame.
        call=api[:1]+emitted(frames,api[1:],symbols)
        tail=emitted(frames,['phw_prefix','phw_quote','phw_head','phw_bytes'],symbols)
        base=prefix+call+tail
        paths[label+' '+kind]=(base,reserve)
        paths[label+' '+kind+' growth']=(base+['ph_buf_reserve','ph_wire_malloc'],reserve)
    if finalize:paths[label+' finalize']=(prefix+['phw_take']+emitted(frames,['phw_discard'],symbols)+['ph_buf_free','ph_wire_free'],reserve)
    return paths


def status_paths(frames,model,label,prefix,reserve,compat=False,capture=True,symbols=None):
    if compat:raise ValueError('JSON compatibility status path is not allowed in MessagePack gate')
    if model not in ('A1','M1','TC1'):raise ValueError('Unknown status model: '+model)
    paths={}
    if capture:
        captures=[model.lower()+'_status_capture']+emitted(frames,[
            'bemfa_status_capture','bemfa_config_flags_get',
            'feature_fan_snapshot','a1_filter_get',
            'tc1_status_begin','tc1_ota_snapshot','reset_name',
            'device_status_text','m1_co2_capture','m1_co2_config_get'],symbols)
        paths[label+' capture']=(prefix+captures,reserve)
    paths[label+' cold sizing init']=(prefix+['device_status_mp','phw_init_measure'],reserve)
    paths[label+' cold sizing finish']=(prefix+['device_status_mp','phw_finish_fixed'],reserve)
    paths[label+' cold sizing buffer']=(prefix+['device_status_mp','ph_buf_reserve','ph_wire_malloc'],reserve)
    schema=prefix+['device_status_mp']+emitted(frames,['device_status_visit'],symbols)+['encode_'+model.lower()]
    # Frozen schema callbacks are tiny and may inline. Sum any emitted schema
    # helpers conservatively; runtime_fields and per-field emitters are siblings.
    schema+=emitted(frames,['runtime_fields','device_http_encode','device_filter_encode',
                           'variant_fields','watchdog_fields'],symbols)
    for kind,primitive in [('string','string'),('int','integer'),('double','number'),
                           ('bool','bool'),('null','null'),('begin','begin')]:
        field=schema+emitted(frames,['emit_'+kind],symbols)
        paths.update(writer_paths(frames,label+' wire '+kind,field,reserve,
                                  symbols,kinds={primitive},finalize=False))
        # The cold sizing walk borrows the same frozen schema and calls the
        # same writer primitives. Its stack is a sibling of emission, never a
        # second retained visitor. Charge the allocator tail conservatively
        # even though the fixed measure writer cannot allocate.
        paths.update(writer_paths(frames,label+' cold sizing wire '+kind,field,reserve,
                                  symbols,kinds={primitive},finalize=False))
    return paths


def diagnostic_paths(frames,model,label,prefix,reserve,symbols=None):
    """On-demand diagnostics capture and encoding are sequential stack tails."""
    if model not in ('A1','M1','TC1'):raise ValueError('Unknown diagnostic model: '+model)
    product=prefix+['service_product_diagnostics_mp']
    capture=product+[model.lower()+'_diagnostics_capture']
    paths={label+' business capture':(capture+[model.lower()+'_status_capture']+
        emitted(frames,['bemfa_status_capture','bemfa_config_flags_get','feature_fan_snapshot','a1_filter_get',
                        'a1_filter_stock_get','feature_values_snapshot','fresh_snapshot',
                        'tc1_status_begin','tc1_ota_snapshot','reset_name','device_status_text',
                        'm1_co2_capture','m1_co2_config_get'],symbols),reserve)}
    if model!='TC1':
        for getter in ['runtime_metrics_capture','watchdog_status_capture','ph_http_snapshot']:
            # Private runtime helper can inline into the capture owner; public
            # watchdog/HTTP entry points are mandatory in a product build.
            tail=emitted(frames,[getter],symbols) if getter=='runtime_metrics_capture' else [getter]
            paths[label+' '+getter]=(capture+tail,reserve)
    paths[label+' service memory']=(product+['service_memory_snapshot'],reserve)
    schema=product+['device_diagnostics_mp']
    paths.update(writer_paths(frames,label+' pair',schema+
        emitted(frames,['diagnostic_field'],symbols),reserve,symbols,kinds={'begin','unsigned'},finalize=False))
    paths.update(writer_paths(frames,label+' value',schema,reserve,symbols,
                              kinds={'string','integer','unsigned','bool','null'},finalize=False))
    # The compact typed [id,value] helpers are sibling calls. An emitted helper
    # must have an actual frame; charge it on both its pair and value tails.
    for helper,kind in [('diagnostic_i32','integer'),('diagnostic_u32','unsigned'),
                        ('diagnostic_bool','bool'),('diagnostic_text','string')]:
        actual=emitted(frames,[helper],symbols)
        if not actual:continue
        call=schema+actual
        paths.update(writer_paths(frames,label+' '+helper+' pair',call+
            emitted(frames,['diagnostic_field'],symbols),reserve,symbols,
            kinds={'begin','unsigned'},finalize=False))
        paths.update(writer_paths(frames,label+' '+helper+' value',call,reserve,
                                  symbols,kinds={kind},finalize=False))
    return paths


def feature_read_paths(frames,model,label,prefix,reserve,symbols=None):
    """Frozen config sizing and writing are siblings, never two snapshots."""
    if model not in ('A1','M1'):return {}
    actual=emitted(frames,['feature_domain_get_mp'],symbols)
    if not actual:return {}
    owner=prefix+['service_product_mp','feature_mp']+actual
    schema=owner+emitted(frames,['feature_domain_config_mp','feature_config_fields_mp',
        'linkage_encode_mp','encode_fields_mp','feature_light_speed'],symbols)
    paths=writer_paths(frames,label+' fields',schema,reserve,symbols)
    live=owner+emitted(frames,['feature_domain_live_mp','feature_average_fields_mp'],symbols)
    paths.update(writer_paths(frames,label+' live fields',live,reserve,symbols))
    for aggregate in ['feature_aggregate','feature_co2_aggregate']:
        tail=emitted(frames,[aggregate],symbols)
        if tail:paths[label+' live '+aggregate]=(live+tail,reserve)
    for name,tail in [('freeze init',['phw_init_fixed']),('freeze copy',['phw_cached_value']+emitted(frames,['phw_prefix','phw_bytes'],symbols)+['ph_buf_reserve','ph_wire_malloc']),('measure init',['phw_init_measure']),('measure finish',['phw_finish_fixed']),
                      ('exact reserve',['ph_buf_reserve','ph_wire_malloc'])]:
        paths[label+' '+name]=(owner+tail,reserve)
    return paths
